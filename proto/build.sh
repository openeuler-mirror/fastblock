protoc --cpp_out=../src/rpc common_msg.proto
protoc --cpp_out=../src/rpc raft_msg.proto
protoc --cpp_out=../src/rpc osd_msg.proto

protoc --cpp_out=../src/monclient/ messages.proto

protoc --gogo_out=../monitor/msg/  messages.proto
