pi_perf_compare.sh
# 用法: sh spi_perf_compare.sh

DEVICE="/dev/spidev0.0"
BENCH="./spi_bench"
KO_PATH="./spi_adapter.ko"

echo "=========================================="
echo " SPI PIO vs Interrupt Performance Compare"
echo "=========================================="

# --- 小数据量测试 (2字节 x 500次) ---
echo ""
echo "--- Test 1: 2 bytes x 500 iterations ---"

rmmod spi_adapter 2>/dev/null
insmod $KO_PATH use_irq=0
sleep 1
echo -n "PIO: "
$BENCH $DEVICE 2 500

rmmod spi_adapter
insmod $KO_PATH use_irq=1
sleep 1
echo -n "IRQ: "
$BENCH $DEVICE 2 500

# --- 中数据量测试 (32字节 x 500次) ---
echo ""
echo "--- Test 2: 32 bytes x 500 iterations ---"

rmmod spi_adapter
insmod $KO_PATH use_irq=0
sleep 1
echo -n "PIO: "
$BENCH $DEVICE 32 500

rmmod spi_adapter
insmod $KO_PATH use_irq=1
sleep 1
echo -n "IRQ: "
$BENCH $DEVICE 32 500

# --- 大数据量测试 (128字节 x 500次) ---
echo ""
echo "--- Test 3: 128 bytes x 500 iterations ---"

rmmod spi_adapter
insmod $KO_PATH use_irq=0
sleep 1
echo -n "PIO: "
$BENCH $DEVICE 128 500

rmmod spi_adapter
insmod $KO_PATH use_irq=1
sleep 1
echo -n "IRQ: "
$BENCH $DEVICE 128 500

echo ""
echo "=========================================="
echo " Test Complete"
echo "=========================================="
