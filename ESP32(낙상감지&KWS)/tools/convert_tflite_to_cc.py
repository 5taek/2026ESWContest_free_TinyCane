import argparse
from pathlib import Path

project_root = Path(__file__).resolve().parents[1]
models_dir = project_root / "models"
dst = models_dir / "fall_model.cc"


def select_source_model(model_name: str | None) -> Path:
    if model_name:
        candidate = Path(model_name)
        if not candidate.is_absolute():
            candidate = models_dir / candidate
        if not candidate.exists():
            raise FileNotFoundError(f"model not found: {candidate}")
        return candidate

    candidates = sorted(models_dir.glob("*.tflite"))
    if not candidates:
        raise FileNotFoundError(f"no .tflite files found in {models_dir}")
    if len(candidates) > 1:
        names = ", ".join(path.name for path in candidates)
        raise ValueError(
            "multiple .tflite files found in models/. "
            f"Move old .tflite files to models/archive/ or pass one file name: {names}"
        )
    return candidates[0]


def build_cc(source_model: Path) -> str:
    data = source_model.read_bytes()
    lines = [
        f"// Generated from {source_model.name}.",
        "// Run tools/convert_tflite_to_cc.py to update this file.",
        "",
        "alignas(16) extern const unsigned char g_fall_model_data[] = {",
    ]

    for i, byte in enumerate(data):
        if i % 12 == 0:
            lines.append("  ")
        lines[-1] += f"0x{byte:02x}, "

    lines.extend(
        [
            "};",
            f"extern const unsigned int g_fall_model_data_len = {len(data)};",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert a .tflite model in models/ to models/fall_model.cc."
    )
    parser.add_argument(
        "model",
        nargs="?",
        help="Optional .tflite file name in models/, or an absolute path.",
    )
    args = parser.parse_args()

    src = select_source_model(args.model)
    generated = build_cc(src)

    if dst.exists() and dst.read_text(encoding="utf-8") == generated:
        print(f"unchanged: {dst}")
        print(f"source: {src}")
        return

    dst.write_text(generated, encoding="utf-8")
    print(f"generated: {dst} from {src} ({src.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
