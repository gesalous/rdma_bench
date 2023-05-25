# rdma_bench
RDMA benmark  supports three different receive paths: 1) Blind polling  2) Targeted polling 3) Receive with interrupts
rdma_bench provides the server and the client libraries to build RDMA-enabled applications. 
# Nida server
Nida server is an RDMA server that provides all three flavors of receiving messages:
1. tpoller: tpoller stands for targetted poller. The server polls receive completion queue and when a completion event arrives it locates which queue pair caused the event. Then, it reads the number of bytes written and locates which portion of the communication buffer contains valid data of the message
2. bpoller: bpoller stands for blind poller. The server polls blindly all communication buffers and detects new messages based on special receive fields in each message.
3. ipoller: pollers polls cq with interrupts.
# Nida client
For each poller there is the corresponding client (tclient, bclient, and iclient).
# Applications
Applicatin will provide only the processing logic of the message.


