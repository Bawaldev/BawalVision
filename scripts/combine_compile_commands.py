import json
from pathlib import Path

workspace_root = Path(__file__).resolve().parent.parent

input_files = [
    workspace_root / "services" / "build" / "compile_commands.json",
    workspace_root / "edge" / "build" / "compile_commands.json"
]
output_file = workspace_root / "build" / "compile_commands.json"

merged_commands = []

for file_path in input_files:
    if file_path.exists():
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, list):
                    build_dir = file_path.parent
                    
                    for entry in data:
                        if "directory" in entry:
                            entry["directory"] = str(Path(entry["directory"]).resolve())

                        if "file" in entry:
                            file_p = Path(entry["file"])
                            if not file_p.is_absolute():
                                base_dir = Path(entry.get("directory", build_dir))
                                entry["file"] = str((base_dir / file_p).resolve())
                            else:
                                entry["file"] = str(file_p.resolve())

                        if "output" in entry:
                            out_p = Path(entry["output"])
                            if not out_p.is_absolute():
                                base_dir = Path(entry.get("directory", build_dir))
                                entry["output"] = str((base_dir / out_p).resolve())
                            else:
                                entry["output"] = str(out_p.resolve())
                        
                        merged_commands.append(entry)
                        
                print(f"[OK] Merged & Normalized: {file_path}")
        except Exception as e:
            print(f"[Error] Failed to read {file_path}: {e}")
    else:
        print(f"[Warning] File not found: {file_path}")

output_file.parent.mkdir(parents=True, exist_ok=True)

with open(output_file, "w", encoding="utf-8") as f:
    json.dump(merged_commands, f, indent=2)

print(f"-> Successfully written combined commands to: {output_file}")