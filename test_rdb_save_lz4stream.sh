#!/bin/bash

# Test script to verify RDB save path with LZ4 streaming compression

# Start valkey-server in background
./src/valkey-server --port 6379 --daemonize yes --loglevel notice --logfile /tmp/valkey-test.log

# Wait for server to start
sleep 2

# Configure LZ4 streaming compression
./src/valkey-cli config set rdb-chunk-compression yes
./src/valkey-cli config set rdb-compression-algorithm lz4-stream

# Add some test data
for i in {1..100}; do
    ./src/valkey-cli set "key:$i" "value_$i" > /dev/null
done

# Save RDB
./src/valkey-cli save

# Check the RDB file
echo "RDB file created:"
ls -lh dump.rdb

# Check if algorithm identifier is in the file (byte 10 should be 0x01 for LZ4-stream)
echo ""
echo "RDB header (first 20 bytes):"
xxd -l 20 dump.rdb

# Check server logs for algorithm information
echo ""
echo "Server logs (algorithm info):"
grep -i "compression algorithm" /tmp/valkey-test.log | tail -5

# Shutdown server
./src/valkey-cli shutdown nosave

echo ""
echo "Test complete!"
