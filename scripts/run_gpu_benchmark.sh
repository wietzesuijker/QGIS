#!/usr/bin/env bash
#
# GPU Raster Rendering Benchmark
# ==============================
#
# Runs the GPU vs CPU raster rendering benchmark and displays results.
#
# Prerequisites:
#   1. QGIS built with tests enabled (cmake -DENABLE_TESTS=ON)
#   2. Test COG downloaded (or uses remote /vsicurl/)
#
# Usage:
#   ./scripts/run_gpu_benchmark.sh [options]
#
# Options:
#   --download-cog    Download the test COG file first (654MB)
#   --build           Build the benchmark test before running
#   --help            Show this help
#
# Example:
#   cd /path/to/QGIS
#   ./scripts/run_gpu_benchmark.sh --download-cog --build
#

set -e

# Configuration
COG_URL="https://s3.us-east-1.amazonaws.com/ds-deck.gl-raster-public/cog/Annual_NLCD_LndCov_2024_CU_C1V1.tif"
COG_LOCAL_PATH="/tmp/qgis-bench-cog.tif"
BUILD_DIR="${BUILD_DIR:-build}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo ""
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo ""
}

print_success() {
    echo -e "${GREEN}$1${NC}"
}

print_warning() {
    echo -e "${YELLOW}$1${NC}"
}

print_error() {
    echo -e "${RED}$1${NC}"
}

show_help() {
    head -30 "$0" | tail -25 | sed 's/^#//'
    exit 0
}

download_cog() {
    print_header "Downloading Test COG"

    if [ -f "$COG_LOCAL_PATH" ]; then
        local size=$(ls -lh "$COG_LOCAL_PATH" | awk '{print $5}')
        print_success "COG already exists: $COG_LOCAL_PATH ($size)"
        return 0
    fi

    echo "Downloading NLCD 2024 COG (654MB)..."
    echo "URL: $COG_URL"
    echo ""

    if command -v curl &> /dev/null; then
        curl -L --progress-bar -o "$COG_LOCAL_PATH" "$COG_URL"
    elif command -v wget &> /dev/null; then
        wget --show-progress -O "$COG_LOCAL_PATH" "$COG_URL"
    else
        print_error "Neither curl nor wget found. Please install one."
        exit 1
    fi

    print_success "Downloaded: $COG_LOCAL_PATH"
}

build_benchmark() {
    print_header "Building Benchmark Test"

    if [ ! -d "$BUILD_DIR" ]; then
        print_error "Build directory not found: $BUILD_DIR"
        echo "Run cmake first or set BUILD_DIR environment variable."
        exit 1
    fi

    echo "Building qgis_rastergpubenchmarktest..."
    cmake --build "$BUILD_DIR" --target qgis_rastergpubenchmarktest

    print_success "Build complete"
}

run_benchmark() {
    print_header "Running GPU Raster Benchmark"

    if [ ! -d "$BUILD_DIR" ]; then
        print_error "Build directory not found: $BUILD_DIR"
        exit 1
    fi

    # Check for test executable
    local test_name="qgis_rastergpubenchmarktest"
    local found_test=$(find "$BUILD_DIR" -name "$test_name" -type f 2>/dev/null | head -1)

    if [ -z "$found_test" ]; then
        print_error "Benchmark test not found. Run with --build first."
        exit 1
    fi

    echo "Test executable: $found_test"
    echo "COG path: $COG_LOCAL_PATH"
    echo ""

    # Check if COG exists
    if [ ! -f "$COG_LOCAL_PATH" ]; then
        print_warning "Local COG not found. Using remote /vsicurl/ path."
        print_warning "For faster benchmarks, run with --download-cog"
        echo ""
    fi

    # Run the benchmark
    cd "$BUILD_DIR"
    ctest -R testqgsrastergpubenchmark --output-on-failure -V

    # Extract and highlight results
    echo ""
    print_header "Benchmark Complete"
    echo "Look for the BENCHMARK COMPARISON SUMMARY above."
    echo ""
    echo "Expected speedup:"
    echo "  - Byte COG (categorical): 3-10x"
    echo "  - Float32 COG (continuous): 1.5-3x"
    echo ""
}

# Parse arguments
DO_DOWNLOAD=false
DO_BUILD=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --download-cog)
            DO_DOWNLOAD=true
            shift
            ;;
        --build)
            DO_BUILD=true
            shift
            ;;
        --help|-h)
            show_help
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            ;;
    esac
done

# Run requested operations
if $DO_DOWNLOAD; then
    download_cog
fi

if $DO_BUILD; then
    build_benchmark
fi

run_benchmark
