#!/usr/bin/env bats

load test_helper

@test "healthcheck basic functionality" {
    # Test that conmon works without any healthcheck configuration
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --log-path /tmp/test.log
    [ "$status" -eq 0 ]
}

@test "healthcheck help shows enable-healthcheck flag" {
    # Test that the help output shows the new healthcheck flag
    run $CONMON_BINARY --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"enable-healthcheck"* ]]
}

@test "healthcheck with bundle path but no annotations" {
    # Create a test bundle without healthcheck annotations
    local bundle_path="/tmp/test-bundle-no-hc-$$"
    mkdir -p "$bundle_path"
    
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {"uid": 0, "gid": 0},
        "args": ["/bin/echo", "test"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {"path": "rootfs", "readonly": true},
    "hostname": "test"
}
EOF
    
    # Test that conmon works without healthcheck annotations (no flag)
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log
    [ "$status" -eq 0 ]
    
    # Test that conmon works with healthcheck flag but no annotations
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --enable-healthcheck --log-path /tmp/test.log
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck with disabled healthcheck annotations" {
    # Create a test bundle with disabled healthcheck
    local bundle_path="/tmp/test-bundle-disabled-hc-$$"
    mkdir -p "$bundle_path"
    
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {"uid": 0, "gid": 0},
        "args": ["/bin/echo", "test"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {"path": "rootfs", "readonly": true},
    "hostname": "test",
    "annotations": {
        "io.podman.healthcheck": "{\"test\":[\"CMD-SHELL\",\"echo healthy\"],\"interval\":10,\"timeout\":5,\"retries\":3,\"start_period\":0}"
    }
}
EOF
    
    # Test that conmon works with disabled healthcheck (no flag)
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log
    [ "$status" -eq 0 ]
    
    # Test that conmon works with healthcheck flag but disabled annotations
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --enable-healthcheck --log-path /tmp/test.log
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}
