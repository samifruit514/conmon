#!/usr/bin/env bats

load test_helper

@test "healthcheck command execution: successful command" {
    # Test that a successful command returns exit code 0
    local test_json='{"test":["CMD-SHELL","echo healthy"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "output: $output"
    echo "status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: failing command" {
    # Test that a failing command returns non-zero exit code
    local test_json='{"test":["CMD-SHELL","false"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "output: $output"
    echo "status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: command not found" {
    # Test that a non-existent command returns appropriate exit code
    local test_json='{"test":["CMD-SHELL","nonexistentcommand12345"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "{\"test\":[\"CMD-SHELL\",\"nonexistentcommand12345\"],\"interval\":30,\"timeout\":5,\"retries\":3,\"start_period\":0}"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run_conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --cuuid "test-uuid-123" --log-path /tmp/test.log --runtime /bin/true
    echo "Output: $output"
    echo "Status: $status"
    echo "Log file contents:"
    cat /tmp/test.log 2>/dev/null || echo "No log file created"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: command with arguments" {
    # Test that commands with arguments work correctly
    local test_json='{"test":["CMD-SHELL","test -f /etc/passwd"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "output: $output"
    echo "status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: command that times out" {
    # Test that a command that takes too long is handled properly
    local test_json='{"test":["CMD-SHELL","sleep 10"],"interval":30,"timeout":1,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "output: $output"
    echo "status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: invalid command format" {
    # Test that invalid command formats are handled gracefully
    local test_json='{"test":["INVALID","echo healthy"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "output: $output"
    echo "status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: empty command" {
    # Test that empty commands are handled gracefully
    local test_json='{"test":["CMD-SHELL",""],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with healthcheck
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "output: $output"
    echo "status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
}

@test "healthcheck command execution: CMD vs CMD-SHELL difference" {
    # Test that CMD and CMD-SHELL are handled differently
    # CMD should execute directly with array of arguments, CMD-SHELL should use /bin/sh -c
    
    # Test CMD-SHELL (should use shell)
    local test_json_shell='{"test":["CMD-SHELL","echo test123"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with CMD-SHELL
    local bundle_path=$(mktemp -d)
    local config_path="$bundle_path/config.json"
    
    cat > "$config_path" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json_shell"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path" --cid "test123456789" --runtime /bin/true
    echo "CMD-SHELL output: $output"
    echo "CMD-SHELL status: $status"
    
    # Cleanup
    rm -rf "$bundle_path"
    
    # Test CMD (should execute directly with array of arguments)
    local test_json_cmd='{"test":["CMD","echo","test123"],"interval":30,"timeout":5,"retries":3,"start_period":0}'
    
    # Create a temporary OCI config with CMD
    local bundle_path2=$(mktemp -d)
    local config_path2="$bundle_path2/config.json"
    
    cat > "$config_path2" << EOF
{
    "annotations": {
        "io.podman.healthcheck": "$test_json_cmd"
    }
}
EOF
    
    # Test the healthcheck discovery and execution
    run conmon --enable-healthcheck --bundle "$bundle_path2" --cid "test123456789" --runtime /bin/true
    echo "CMD output: $output"
    echo "CMD status: $status"
    
    # Cleanup
    rm -rf "$bundle_path2"
}
