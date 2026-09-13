#!/usr/bin/env bash
# One-command rebuild + verify loop for the SWC adapter enclosure.
#
#   ./build.sh            full rebuild, verify, export and slice
#   ./build.sh check      parameters + assertions only
#   ./build.sh verify     build + interference / containment / envelope gates
#   ./build.sh export     build + STL/STEP + mesh watertightness
#   ./build.sh slice      export, then slice both parts with OrcaSlicer
#
set -euo pipefail
cd "$(dirname "$0")"

FREECADCMD="/Applications/FreeCAD.app/Contents/Resources/bin/freecadcmd"
ORCA="/Applications/OrcaSlicer.app/Contents/MacOS/OrcaSlicer"
PROF="/Applications/OrcaSlicer.app/Contents/Resources/profiles/Elegoo"
MACHINE="$PROF/machine/ECC/Elegoo Centauri Carbon 0.4 nozzle.json"
PROCESS="$PROF/process/ECC/0.20mm Standard @Elegoo CC 0.4 nozzle.json"
# `--load-filaments` does NOT follow `inherits`: Orca fills every unset key
# from its DEFAULT (PLA) definition, so handing it the vendor profile directly
# slices ASA with a 35 C bed, a PLA chamber-fan branch in the start gcode, and
# `filament_type = PLA` in the header.  Resolve the chain ourselves instead.
FILAMENT_SRC=(
  "$PROF/filament/fdm_filament_asa.json"
  "$PROF/filament/BASE/Elegoo ASA @base.json"
  "$PROF/filament/ECC/Elegoo ASA @ECC.json"
)
FILAMENT="$(pwd)/.asa_resolved.json"

# Flatten the ASA profile chain (leaf last) into one self-contained profile.
resolve_filament() {
  python3 - "${FILAMENT_SRC[@]}" "$FILAMENT" <<'PY'
import json, sys
out = {}
for p in sys.argv[1:-1]:                 # leaf last wins
    out.update(json.load(open(p)))
out.pop("inherits", None)                # nothing left to inherit
out["from"] = "User"
out["name"] = out["filament_settings_id"] = "SWC ASA (resolved)"
json.dump(out, open(sys.argv[-1], "w"), indent=1)
PY
}

# strip FreeCAD's noisy OCCT trace lines from logs
_filter() { grep -vE "OCCT\(trace\)|Step File|STEP Loading|records \(entities|Parameters prepared|Objects analysed|^ +\.\.\.|AICopilot|FreeCAD is free|^\(C\) 2001|^FreeCAD 26"; }

# Run a module's `main()` by name.  `freecadcmd file.py` never sets
# `__name__ == "__main__"`, so the usual guard at the bottom of each module is
# dead code under this runner -- invoking the file directly silently does
# nothing and exits 0.  That is how `build.sh check` became a no-op.
step_run() {
  # Split these: bash expands every RHS in a single `local` before performing
  # any of the assignments, so `${mod}` would read the *outer* (unset) name and
  # trip `set -u`.
  local mod="$1"
  local tmp="/tmp/.swc_step_${mod}.py"
  # Needs a real `.py` suffix -- freecadcmd rejects an extensionless file with
  # "File format not supported".
  printf 'import sys; sys.path.insert(0, %s); import %s; %s.main()\n' \
         "'$(pwd)'" "$mod" "$mod" > "$tmp"
  "$FREECADCMD" "$tmp"; local rc=$?; rm -f "$tmp"; return $rc
}

step_check()  { echo "### casegeom assertions";  step_run casegeom; }
step_verify() { echo "### build + verification gates"; step_run verify; }
step_export() { echo "### build + export"; step_run export; }
step_assembly() { echo "### assembly document"; step_run make_assembly; }

step_slice() {
  echo "### OrcaSlicer slice (Elegoo Centauri Carbon, 0.4 mm, ASA)"
  resolve_filament
  rm -rf slice_out && mkdir -p slice_out
  # Case_Body and Case_Lid are the two structural parts; slice them together
  # with auto-arrange.  Logo_Inlay is NOT sliced with them: it prints as the
  # second material of the lid, co-located in the lid's pocket.  To print the
  # two-colour lid, load Case_Lid.stl AND Logo_Inlay.stl on ONE plate with
  # arrange turned OFF (both STLs are already print-oriented and stay in
  # registration), assign the inlay the second filament, and add a filament
  # change at the layer where the inlay's top face ends.
  "$ORCA" --load-settings "$MACHINE;$PROCESS" --load-filaments "$FILAMENT" \
          --arrange 1 --slice 1 --outputdir "$(pwd)/slice_out" \
          Case_Body.stl Case_Lid.stl 2>/dev/null
  local g="slice_out/plate_1.gcode"
  [ -f "$g" ] || { echo "SLICE FAILED: no gcode produced"; exit 1; }
  # The ASA profile resolved or it did not.  A silent fall back to PLA would
  # ship a 35 C bed and a PLA chamber-fan branch, so make it loud.
  local ftype ftemp
  ftype=$(grep -m1 '; filament_type =' "$g" | sed 's/.*= *//')
  ftemp=$(grep -m1 '; hot_plate_temp =' "$g" | sed 's/.*= *//')
  [ "$ftype" = "ASA" ] || { echo "SLICE FAILED: filament resolved as '$ftype', not ASA"; exit 1; }
  echo "  $(grep -c '^;LAYER_CHANGE' "$g") layers, $(grep -m1 'max_z_height' "$g"), $(du -h "$g" | cut -f1)"
  echo "  filament: $ftype @ bed ${ftemp} C, $(grep -m1 '; nozzle_temperature =' "$g" | sed 's/.*= *//' | tr -d '[],' ) C nozzle"
  echo "  gcode: $g"

  # --- logo inlay on its own plate: proves the mark slices as a solid mark
  rm -rf slice_logo && mkdir -p slice_logo
  "$ORCA" --load-settings "$MACHINE;$PROCESS" --load-filaments "$FILAMENT" \
          --arrange 1 --slice 1 --outputdir "$(pwd)/slice_logo" \
          Logo_Inlay.stl 2>/dev/null
  local g2="slice_logo/plate_1.gcode"
  if [ -f "$g2" ]; then
    echo "  Logo_Inlay: $(grep -c '^;LAYER_CHANGE' "$g2") layers, $(du -h "$g2" | cut -f1)"
    echo "  gcode: $g2"
  else
    echo "  WARNING: Logo_Inlay did not slice"
  fi
}

case "${1:-all}" in
  check)    step_check ;;
  verify)   step_verify ;;
  export)   step_export ;;
  assembly) step_assembly ;;
  slice)    step_export; step_slice ;;
  all)      step_check; step_verify; step_export; step_assembly; step_slice ;;
  *) echo "usage: $0 [check|verify|export|assembly|slice|all]"; exit 2 ;;
esac
echo "### done"
