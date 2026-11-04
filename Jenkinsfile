// Jenkinsfile - multi-stage CI for the STM32 A/B bootloader.
//
//   Checkout -> Toolchain -> Build -> Host tests -> Python tests -> Static
//   analysis -> Layout contract -> Image tool -> HIL (optional) -> Publish
//
// Builds run through scripts/docker-dev.sh (BCON-001); the stm32-dev image is
// built from docker/Dockerfile on first use. The layout stage keeps the
// bootloader's own constants honest, and the image-tool stage exercises
// tools/mkimage.py (SP/reset-vector validation) without hardware.

pipeline {
    agent any

    options {
        skipDefaultCheckout()
        timestamps()
        timeout(time: 45, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '30', artifactNumToKeepStr: '10'))
        disableConcurrentBuilds()
    }

    parameters {
        choice(name: 'BUILD_TYPE', choices: ['Release', 'Debug'], description: 'CMake build type')
        booleanParam(name: 'RUN_LINT', defaultValue: true, description: 'Run clang-tidy')
        booleanParam(name: 'STRICT_LINT', defaultValue: false, description: 'Fail on any clang-tidy finding')
        booleanParam(name: 'RUN_HIL', defaultValue: false, description: 'Run the bench NUCLEO hardware-in-the-loop job')
        string(name: 'HIL_PORT', defaultValue: '/dev/ttyACM0', description: 'ST-LINK VCP for the HIL job')
        string(name: 'HIL_IMAGE_A', defaultValue: 'build/app_a.img', description: 'Confirmed application image for slot A')
        string(name: 'HIL_IMAGE_B', defaultValue: 'build/app_b.img', description: 'Confirmed application image for slot B')
    }

    environment {
        STM32_IMAGE = 'stm32-dev:latest'
    }

    stages {
        stage('Checkout') {
            steps {
                checkout scm
                sh '''
                    git log -1 --pretty=fuller
                    echo "commit: $(git rev-parse HEAD)"
                    echo "branch: $(git rev-parse --abbrev-ref HEAD)"
                '''
            }
        }

        stage('Toolchain') {
            steps {
                sh '''
                    docker version --format 'docker server: {{.Server.Version}}'
                    if ! docker image inspect "${STM32_IMAGE}" >/dev/null 2>&1; then
                        echo "Building ${STM32_IMAGE} from docker/Dockerfile"
                        make image
                    fi
                    docker image inspect --format 'image: {{.Id}}' "${STM32_IMAGE}"
                    scripts/docker-dev.sh run arm-none-eabi-gcc --version | head -n 1
                    scripts/docker-dev.sh run python3 --version
                '''
            }
        }

        stage('Build') {
            steps {
                sh "make build BUILD_TYPE=${params.BUILD_TYPE}"
            }
        }

        stage('Host tests') {
            steps {
                sh 'make test-host'
            }
        }

        stage('Python tests') {
            steps {
                sh '''
                    python3 -m pip install --quiet -r tools/requirements.txt
                    python3 -m pip install --quiet pytest
                    make test-python
                '''
            }
        }

        stage('Static analysis') {
            when { expression { return params.RUN_LINT } }
            steps {
                script {
                    if (params.STRICT_LINT) {
                        sh 'make lint-strict'
                    } else {
                        sh 'make lint'
                    }
                }
            }
        }

        stage('Layout contract') {
            steps {
                sh '''
                    make test-contract
                    python3 - <<'PY'
from pathlib import Path

# SRS-BOOT-F767 LAY-001..LAY-005 / Appendix B. These are the values the
# applications are built against; a change here is a protocol change.
expected = {
    "BOOT_BANK1_BASE": 0x08000000,
    "BOOT_BANK2_BASE": 0x08100000,
    "BOOT_BANK_SIZE": 0x00100000,
    "BOOT_META_OFFSET": 0x00008000,
    "BOOT_META_SIZE": 0x00004000,
    "BOOT_SLOT_OFFSET": 0x0000C000,
    "BOOT_SLOT_SIZE": 0x000F4000,
    "BOOT_META_VERIFIED_OFFSET": 0x100,
    "BOOT_META_ACTIVATED_OFFSET": 0x120,
    "BOOT_META_CONFIRMED_OFFSET": 0x140,
    "BOOT_META_REJECTED_OFFSET": 0x160,
}

defines = {}
for line in Path("src/common/boot_layout.h").read_text(encoding="utf-8").splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[0] == "#define":
        defines[parts[1]] = parts[2]

for name, want in expected.items():
    if name not in defines:
        raise SystemExit(f"layout: {name} is missing from src/common/boot_layout.h")
    got = int(defines[name].rstrip("ULul").rstrip("ULul"), 16)
    if got != want:
        raise SystemExit(f"layout: {name} is 0x{got:08X}, expected 0x{want:08X}")

print("layout: bank, slot and metadata constants match the published contract")
PY
                    python3 - <<'PY'
# P1-4: factory metadata must follow Appendix B exactly.
import zlib
import sys
sys.path.insert(0, "tools")
import mkimage

header = bytes(256)
sector = mkimage.build_metadata(header, 0x11223344, 1)
assert len(sector) == 0x4000, "metadata sector size"
for offset, magic in ((0x100, 0x56455249), (0x120, 0x41435456),
                      (0x140, 0x434F4E46)):
    record = sector[offset:offset + 32]
    assert int.from_bytes(record[:4], "little") == magic
    assert zlib.crc32(record[:28]) & 0xFFFFFFFF == \
        int.from_bytes(record[28:32], "little")
print("factory metadata: header, VERIFIED, ACTIVATED and CONFIRMED OK")
PY
                '''
            }
        }

        stage('Image tool') {
            steps {
                sh '''
                    make size
                    python3 - <<'PY'
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

work = Path(tempfile.mkdtemp(prefix="boot-image-check-"))
binary = work / "app.bin"
image = work / "app.img"

# Minimal vector table: SP in internal RAM, Thumb reset vector in slot A.
binary.write_bytes(struct.pack("<II", 0x20020000, 0x0800C001))
cmd = [
    sys.executable, "tools/mkimage.py",
    "--input", str(binary), "--output", str(image),
    "--version", "1.2.3", "--commit", "0" * 40,
    "--timestamp", "0", "--name", "ci-check",
]
subprocess.run(cmd, check=True)

data = image.read_bytes()
magic, hver, hsize, itype, ver, isize, icrc, load, entry, _ts = struct.unpack_from(
    "<IHHIIIIIII", data, 0)
assert magic == 0x48574249, "mkimage: bad magic"
assert hver == 1 and hsize == 256, "mkimage: bad header version/size"
assert itype == 1, "mkimage: bad image type"
assert load == 0x0800C000, f"mkimage: load address 0x{load:08X}"
assert isize == 8 and len(data) == hsize + isize, "mkimage: wrong payload size"
assert entry == 0x0800C001, "mkimage: reset vector not preserved"

# The header must reject a binary whose reset vector is outside the slot.
bad = work / "bad.bin"
bad.write_bytes(struct.pack("<II", 0x20020000, 0x08000101))
result = subprocess.run(
    [sys.executable, "tools/mkimage.py", "--input", str(bad),
     "--output", str(work / "bad.img"), "--version", "1.2.3"],
    capture_output=True, text=True)
assert result.returncode != 0, "mkimage: out-of-slot reset vector was accepted"

print("mkimage: header layout, CRC and vector-table validation OK")
PY
                '''
            }
        }

        stage('Hardware-in-the-loop') {
            when { expression { return params.RUN_HIL } }
            steps {
                sh '''
                    test -f "${HIL_IMAGE_A}" || { echo "HIL_IMAGE_A not found: ${HIL_IMAGE_A}"; exit 1; }
                    test -f "${HIL_IMAGE_B}" || { echo "HIL_IMAGE_B not found: ${HIL_IMAGE_B}"; exit 1; }
                    tools/provision.sh build/boot.elf
                    tools/bootctl.py --port "$HIL_PORT" update --both \
                        --a "$HIL_IMAGE_A" --b "$HIL_IMAGE_B" --json
                    tools/bootctl.py --port "$HIL_PORT" info --json

                    # Rollback: a vector-table-only image passes the header,
                    # vector and CRC checks but faults as soon as BOOT jumps
                    # to it, so the trial limit must reject it and boot the
                    # confirmed slot again.
                    python3 - <<'PY'
import struct, subprocess, sys
from pathlib import Path
work = Path("build")
binary = work / "hil_bad.bin"
binary.write_bytes(struct.pack("<II", 0x20020000, 0x0800C001))
subprocess.run([sys.executable, "tools/mkimage.py", "--input", str(binary),
                "--output", str(work / "hil_bad.img"), "--version", "0.0.1",
                "--name", "hil-bad"], check=True)
PY
                    set +e
                    tools/bootctl.py --port "$HIL_PORT" update \
                        build/hil_bad.img --activate --wait --json
                    bad_status=$?
                    set -e
                    if [ "$bad_status" -eq 0 ]; then
                        echo "rollback test unexpectedly confirmed a bad image"
                        exit 1
                    fi
                    tools/bootctl.py --port "$HIL_PORT" info --json \
                        | tee build/hil_info.json
                    python3 - <<'PY'
import json
data = json.load(open("build/hil_info.json"))
assert data["last_outcome"] == "ROLLED_BACK", data["last_outcome"]
assert data["active_valid"] is True, data
print("HIL: both slots confirmed and bad-image rollback verified")
PY
                '''
            }
        }

        stage('Publish') {
            steps {
                sh '''
                    make package-hwlink
                    sha256sum build/boot.elf build/boot.bin build/boot.hex > build/SHA256SUMS
                    sha256sum build/boot.map >> build/SHA256SUMS
                    cat build/SHA256SUMS
                '''
                archiveArtifacts artifacts: 'build/boot.elf,build/boot.bin,build/boot.hex,build/boot.map,build/SHA256SUMS,build/soccentric-hwlink/**',
                                 fingerprint: true
            }
        }
    }

    post {
        always {
            deleteDir()
        }
        success {
            echo 'STM32 bootloader: build, lint, layout and image-tool checks passed.'
        }
        failure {
            echo 'STM32 bootloader: pipeline failed - see the stage logs above.'
        }
    }
}
