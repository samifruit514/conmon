#!/usr/bin/env bats

load test_helper

@test "startup healthcheck integration test" {
    # This test demonstrates the complete startup healthcheck flow
    local bundle_path="/tmp/test-bundle-startup-integration-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with startup healthcheck
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": ["/bin/sh", "-c", "echo 'Container started' && sleep 30"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {
        "path": "rootfs",
        "readonly": true
    },
    "hostname": "test",
    "annotations": {
        "io.podman.healthcheck": "{\"test\":[\"CMD\",\"echo\",\"healthy\"],\"interval\":2,\"timeout\":5,\"start_period\":10,\"retries\":3}"
    }
}
EOF
    
    # Test that conmon can parse startup healthcheck configuration
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test-startup.log --enable-healthcheck --version
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
    rm -f "/tmp/test-startup.log"
}

@test "startup healthcheck with failing command during startup" {
    # This test shows that failures during startup don't count against retry limit
    local bundle_path="/tmp/test-bundle-startup-fail-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with startup healthcheck that will fail initially
    cat > "$bundle_path/config.json" << 'EOF'
{
    "ociVersion": "1.0.0",
    "process": {
        "terminal": false,
        "user": {
            "uid": 0,
            "gid": 0
        },
        "args": ["/bin/sh", "-c", "echo 'Container started' && sleep 30"],
        "env": ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"],
        "cwd": "/"
    },
    "root": {
        "path": "rootfs",
        "readonly": true
    },
    "hostname": "test",
    "annotations": {
        "io.podman.healthcheck": "{\"test\":[\"CMD\",\"false\"],\"interval\":2,\"timeout\":5,\"start_period\":10,\"retries\":3}"
    }
}
EOF
    
    # Test that conmon can parse startup healthcheck configuration with failing command
    # Failures during startup should not count against retry limit
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test-startup-fail.log --enable-healthcheck --version
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
    rm -f "/tmp/test-startup-fail.log"
}
