#!/usr/bin/env bats

load test_helper

@test "startup healthcheck with start period" {
    # Create a test bundle with startup healthcheck
    local bundle_path="/tmp/test-bundle-startup-hc-$$"
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
        "io.podman.healthcheck": "{\"test\":[\"CMD\",\"echo\",\"healthy\"],\"interval\":5,\"timeout\":10,\"start_period\":15,\"retries\":3}"
    }
}
EOF
    
    # Test that conmon can parse startup healthcheck configuration
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "startup healthcheck without start period" {
    # Create a test bundle with healthcheck but no start period
    local bundle_path="/tmp/test-bundle-no-startup-hc-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with healthcheck but no start period
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
        "io.podman.healthcheck": "{\"test\":[\"CMD\",\"echo\",\"healthy\"],\"interval\":5,\"timeout\":10,\"retries\":3}"
    }
}
EOF
    
    # Test that conmon can parse healthcheck configuration without start period
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "startup healthcheck with invalid JSON" {
    # Create a test bundle with invalid healthcheck JSON
    local bundle_path="/tmp/test-bundle-invalid-hc-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with invalid healthcheck JSON
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
        "io.podman.healthcheck": "{\"test\":[\"CMD\",\"echo\",\"healthy\"],\"interval\":5,\"timeout\":10,\"start_period\":15,\"retries\":3"
    }
}
EOF
    
    # Test that conmon handles invalid JSON gracefully
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "startup healthcheck with missing test command" {
    # Create a test bundle with healthcheck but no test command
    local bundle_path="/tmp/test-bundle-no-test-hc-$$"
    mkdir -p "$bundle_path"
    
    # Create a config.json with healthcheck but no test command
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
        "io.podman.healthcheck": "{\"interval\":5,\"timeout\":10,\"start_period\":15,\"retries\":3}"
    }
}
EOF
    
    # Test that conmon handles missing test command gracefully
    run $CONMON_BINARY --cid test-container --cuuid test-uuid --runtime /bin/echo --bundle "$bundle_path" --log-path /tmp/test.log --enable-healthcheck
    [ "$status" -eq 0 ]
    
    # Cleanup
    rm -rf "$bundle_path"
}
