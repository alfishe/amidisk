#!/bin/bash
set -e

cd /Volumes/TB4-4Tb/Projects/Amiga/software/AmigaFSTool

CPP_BIN="amidisk_cpp/build/bin/amidisk"
TAR_FILE="scratch/dummy.tar"
EXTRACT_DIR="/tmp/amidisk_bench_extract"

echo "=============================================="
echo "AmigaFSTool Benchmark: C++ vs Python"
echo "=============================================="
echo ""
echo "Test archive: $TAR_FILE ($(du -h $TAR_FILE | cut -f1))"
echo "Files in archive: $(tar tf $TAR_FILE 2>/dev/null | wc -l | tr -d ' ')"
echo ""

# Filesystems to test
declare -a FILESYSTEMS=("ffs" "ffs-intl" "pfs3" "sfs")
declare -a DOSTYPES_CPP=("444F5301" "444F5303" "50465303" "53465300")
declare -a DOSTYPES_PY=("ffs" "ffs-intl" "pfs3" "sfs")

# Results arrays
declare -a CPP_TIMES=()
declare -a CPP_BULK_TIMES=()
declare -a PY_TIMES=()
declare -a PY_BULK_TIMES=()

run_benchmark() {
    local fs_name=$1
    local dostype_cpp=$2
    local dostype_py=$3
    local idx=$4

    echo "----------------------------------------"
    echo "Testing: $fs_name"
    echo "----------------------------------------"

    # Cleanup
    rm -f /tmp/bench_cpp.hdf /tmp/bench_py.hdf
    rm -rf "$EXTRACT_DIR"
    mkdir -p "$EXTRACT_DIR/cpp" "$EXTRACT_DIR/py"

    # C++ setup
    $CPP_BIN create /tmp/bench_cpp.hdf -s 100 >/dev/null
    $CPP_BIN rdb-init /tmp/bench_cpp.hdf >/dev/null
    $CPP_BIN part-add /tmp/bench_cpp.hdf DH0 -s 80 -t $dostype_cpp >/dev/null
    $CPP_BIN format /tmp/bench_cpp.hdf:DH0 Benchmark >/dev/null

    # C++ write (no bulk)
    echo -n "  C++ write (no bulk): "
    START=$(python3 -c "import time; print(time.time())")
    $CPP_BIN cp /tmp/bench_cpp.hdf:DH0 "$TAR_FILE" / >/dev/null 2>&1
    END=$(python3 -c "import time; print(time.time())")
    CPP_TIME=$(python3 -c "print(f'{$END - $START:.3f}')")
    echo "${CPP_TIME}s"
    CPP_TIMES[$idx]=$CPP_TIME

    # C++ check
    echo -n "  C++ check: "
    $CPP_BIN check /tmp/bench_cpp.hdf:DH0 2>&1 | grep -E "files:|state:" | tr '\n' ' '
    echo ""

    # C++ extract and checksum
    echo -n "  C++ extract: "
    $CPP_BIN extract /tmp/bench_cpp.hdf:DH0 -r "$EXTRACT_DIR/cpp" >/dev/null 2>&1
    CPP_MD5=$(find "$EXTRACT_DIR/cpp" -type f -exec md5 -q {} \; 2>/dev/null | sort | md5 -q)
    echo "checksum=$CPP_MD5"

    # Reset for bulk test
    rm -f /tmp/bench_cpp.hdf
    $CPP_BIN create /tmp/bench_cpp.hdf -s 100 >/dev/null
    $CPP_BIN rdb-init /tmp/bench_cpp.hdf >/dev/null
    $CPP_BIN part-add /tmp/bench_cpp.hdf DH0 -s 80 -t $dostype_cpp >/dev/null
    $CPP_BIN format /tmp/bench_cpp.hdf:DH0 Benchmark >/dev/null

    # C++ write (with bulk) - bulk mode not yet implemented for cp, skip
    CPP_BULK_TIMES[$idx]="N/A"

    # Python setup
    python3 -m amidisk create /tmp/bench_py.hdf --size 100M >/dev/null 2>&1
    python3 -m amidisk rdb-init /tmp/bench_py.hdf >/dev/null 2>&1
    python3 -m amidisk part-add /tmp/bench_py.hdf DH0 --size 80M --dostype $dostype_py >/dev/null 2>&1
    python3 -m amidisk format /tmp/bench_py.hdf:DH0 Benchmark >/dev/null 2>&1

    # Python write (no bulk)
    echo -n "  Python write (no bulk): "
    START=$(python3 -c "import time; print(time.time())")
    python3 -m amidisk cp "$TAR_FILE" /tmp/bench_py.hdf:DH0/ >/dev/null 2>&1
    END=$(python3 -c "import time; print(time.time())")
    PY_TIME=$(python3 -c "print(f'{$END - $START:.3f}')")
    echo "${PY_TIME}s"
    PY_TIMES[$idx]=$PY_TIME

    # Python check
    echo -n "  Python check: "
    python3 -m amidisk check /tmp/bench_py.hdf:DH0 2>&1 | head -1

    # Python extract and checksum
    echo -n "  Python extract: "
    rm -rf "$EXTRACT_DIR/py"
    mkdir -p "$EXTRACT_DIR/py"
    python3 -m amidisk extract /tmp/bench_py.hdf:DH0 -r "$EXTRACT_DIR/py" >/dev/null 2>&1
    PY_MD5=$(find "$EXTRACT_DIR/py" -type f -exec md5 -q {} \; 2>/dev/null | sort | md5 -q)
    echo "checksum=$PY_MD5"

    # Reset for bulk test
    rm -f /tmp/bench_py.hdf
    python3 -m amidisk create /tmp/bench_py.hdf --size 100M >/dev/null 2>&1
    python3 -m amidisk rdb-init /tmp/bench_py.hdf >/dev/null 2>&1
    python3 -m amidisk part-add /tmp/bench_py.hdf DH0 --size 80M --dostype $dostype_py >/dev/null 2>&1
    python3 -m amidisk format /tmp/bench_py.hdf:DH0 Benchmark >/dev/null 2>&1

    # Python write (with bulk)
    echo -n "  Python write (bulk): "
    START=$(python3 -c "import time; print(time.time())")
    python3 -m amidisk cp "$TAR_FILE" /tmp/bench_py.hdf:DH0/ --bulk >/dev/null 2>&1
    END=$(python3 -c "import time; print(time.time())")
    PY_BULK_TIME=$(python3 -c "print(f'{$END - $START:.3f}')")
    echo "${PY_BULK_TIME}s"
    PY_BULK_TIMES[$idx]=$PY_BULK_TIME

    # Verify checksums match
    if [ "$CPP_MD5" = "$PY_MD5" ]; then
        echo "  Checksum MATCH"
    else
        echo "  WARNING: Checksum MISMATCH! C++=$CPP_MD5 Python=$PY_MD5"
    fi

    echo ""
}

# Run benchmarks
for i in "${!FILESYSTEMS[@]}"; do
    run_benchmark "${FILESYSTEMS[$i]}" "${DOSTYPES_CPP[$i]}" "${DOSTYPES_PY[$i]}" $i
done

# Print summary table
echo "=============================================="
echo "BENCHMARK SUMMARY"
echo "=============================================="
echo ""
printf "%-12s | %12s | %12s | %12s | %8s\n" "Filesystem" "C++ (no bulk)" "Python" "Python+bulk" "Speedup"
printf "%-12s-+-%12s-+-%12s-+-%12s-+-%8s\n" "------------" "------------" "------------" "------------" "--------"

for i in "${!FILESYSTEMS[@]}"; do
    SPEEDUP=$(python3 -c "print(f'{${PY_TIMES[$i]} / ${CPP_TIMES[$i]}:.1f}x')" 2>/dev/null || echo "N/A")
    printf "%-12s | %11ss | %11ss | %11ss | %8s\n" \
        "${FILESYSTEMS[$i]}" "${CPP_TIMES[$i]}" "${PY_TIMES[$i]}" "${PY_BULK_TIMES[$i]}" "$SPEEDUP"
done

echo ""
echo "Note: C++ bulk mode not yet implemented for cp command"

# Cleanup
rm -f /tmp/bench_cpp.hdf /tmp/bench_py.hdf
rm -rf "$EXTRACT_DIR"
