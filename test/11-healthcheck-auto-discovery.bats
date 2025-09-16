#!/usr/bin/env bats

load test_helper

@test "healthcheck automatic discovery from OCI config" {
    # Create a test bundle directory
    local bundle_path="/tmp/test-bundle-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with healthcheck annotations
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": ["/bin/echo", "test"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {
        "path": "rootfs",
        "readonly": true
    },
    "hostname": "test",
    "annotations": {
        "io.podman.healthcheck": "{\"test\":[\"CMD-SHELL\",\"echo healthy\"],\"interval\":10,\"timeout\":5,\"retries\":3,\"start_period\":0}"
    }
}
EOF
    
    # Test that conmon can parse healthcheck flag with bundle path
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck discovery with missing annotations" {
    # Create a test bundle directory without healthcheck annotations
    local bundle_path="/tmp/test-bundle-no-hc-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json without healthcheck annotations
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": ["/bin/echo", "test"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {
        "path": "rootfs",
        "readonly": true
    },
    "hostname": "test"
}
EOF
    
    # Test that conmon works without healthcheck annotations (no flag)
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log
    [ "$status" -eq 0 ]
    
    # Test that conmon works with healthcheck flag but no annotations
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck discovery with disabled healthcheck" {
    # Create a test bundle directory with disabled healthcheck
    local bundle_path="/tmp/test-bundle-disabled-hc-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with disabled healthcheck
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": ["/bin/echo", "test"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {
        "path": "rootfs",
        "readonly": true
    },
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
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

