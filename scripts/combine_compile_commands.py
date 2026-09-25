import json
from pathlib import Path

workspace_root = Path(__file__).resolve().parent.parent

input_files = [
    workspace_root / "services/build/compile_commands.json",
    workspace_root / "edge/build/compile_commands.json"
]
output_file = workspace_root / "build/compile_commands.json"

merged_commands = []

for file_path in input_files:
    if file_path.exists():
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, list):
                    merged_commands.extend(data)
                print(f"[OK] Merged: {file_path}")
        except Exception as e:
            print(f"[Error] Failed to read {file_path}: {e}")
    else:
        print(f"[Warning] File not found: {file_path}")

output_file.parent.mkdir(parents=True, exist_ok=True)

with open(output_file, "w", encoding="utf-8") as f:
    json.dump(merged_commands, f, indent=2)

print(f"-> Successfully written combined commands to: {output_file}")