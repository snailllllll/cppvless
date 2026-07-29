import sys

path = '/root/vmess_src_deploy_20260501005934/vmess/src/server/vless_connection.cpp'
with open(path, 'r') as f:
    lines = f.readlines()

new_func = '''void VlessConnection::enterRelayState(int targetFd) {
    targetFd_ = targetFd;
    state_ = State::RELAY;
    std::cout << "[VlessConnection] Entering RELAY state, targetFd=" << targetFd << std::endl;

    auto remaining = stream_.drainRemaining();
    if (!remaining.empty()) {
        handshakeRemaining_.assign(remaining.begin(), remaining.end());
        pendingSends_.push_back({targetFd_, handshakeRemaining_.data(), handshakeRemaining_.size()});
        std::cout << "[VlessConnection] Handshake remaining data queued: " << handshakeRemaining_.size()
                  << " bytes -> targetFd=" << targetFd << std::endl;
    }
}
'''

with open(path, 'w') as f:
    f.writelines(lines[:295])
    f.write(new_func)
    f.writelines(lines[297:])

print('OK')
