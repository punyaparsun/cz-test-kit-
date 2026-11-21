# Vishwakarma CZ + Nano-Bytes File Engine (Linux C++17)

Standalone Linux-compatible C++17 reference prototype for `.cz` file packing, restoration (`--pack`, `--unpack`), deterministic `Nano-Bytes`, and `vcore` / `vme::Runtime` evaluation.

## Requirements
- Linux (Ubuntu / Debian / Fedora / Arch, or Google Colab / cloud Linux shell)
- `g++` with C++17 support (`build-essential`)
- `python3` and `sha256sum` (used by `test_run.sh` for sample generation and hash checks)

### Ubuntu / Debian Setup
```bash
sudo apt update
sudo apt install -y build-essential

Build
Give executable permissions to the scripts and compile the C++17 binary:

chmod +x build.sh test_run.sh
./build.sh


Run Tests
1. Quick Synthetic Test (No Input File Needed)
Bash
./test_run.sh





2. Test on Your Own File (e.g., 300 MB or movie.mp4)

./test_run.sh ./vishwakarma_cz_engine /path/to/your_file.mp4




Manual CLI Commands
Pack a File to .cz


./vishwakarma_cz_engine --pack input_file.bin output_file.cz
ls -lh input_file.bin output_file.cz


Unpack / Restore .cz & Verify SHA-256
Bash


./vishwakarma_cz_engine --unpack output_file.cz restored_file.bin
sha256sum input_file.bin restored_file.bin




Read Deterministic Nano-Bytes


./vishwakarma_cz_engine --nano-read 0 64




Evaluation Note
Provided for temporary technical evaluation in a Linux environment. Please share actual output sizes, execution time, and SHA-256 verification results after testing.
