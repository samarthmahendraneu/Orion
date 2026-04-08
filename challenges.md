# Challenges: Moving to a Distributed Architecture

Building a "distributed" system like Orion is much harder than building a regular app that runs on one computer. Here are the biggest problems I ran into and how I solved them.

### 1. The "Wait, Who Has the File?" Problem
**Challenge**: On one computer, if Part A builds a file, Part B can see it immediately on the hard drive. In a cluster, Part A might be on Node 1 and Part B on Node 4. Node 4 has no idea the file exists.
**Tackle**: I built a **Global Task Tracker** on the "Head" node. Now, every node "calls home" after a task finishes. The Head node keeps a master list of everything that’s ready and only tells other nodes to start when their dependencies are officially finished.

### 2. The Local Deadlock (The trickiest bug)
**Challenge**: I found that nodes were getting stuck forever. Why? Because I had two "Schedulers" fighting each other. The Master Scheduler would say "Go!" but the Local Scheduler on the node would say "Wait, I don't see the file locally yet" and block the task.
**Tackle**: I changed the logic so that if a task comes from the Master Head, the local node ignores its own dependency checks. We trust the "Head" node’s global view. This instantly fixed the "hanging" builds.

### 3. Talking via the Network (gRPC)
**Challenge**: Computers in a cluster need to talk to each other. I used **gRPC**, but at first, it was very slow because I was opening a new "phone call" (connection) for every single tiny update. This exhausted the computer's resources.
**Tackle**: I optimized this by making the nodes keep the "phone line" open (cached connections). Now reporting a finished task is almost instant.

### 4. Making it "Universal"
**Challenge**: At first, I had to write new C++ code inside the Orion framework every time I wanted to compile a different project. This isn't how a real build system works.
**Tackle**: I added a "Universal Shell" function. Now, Orion can run ANY command (like `clang++`, `make`, or `cmake`) just by receiving a string. This made the system work for any project without ever touching the core code again.

### 5. Speed vs. Noise
**Challenge**: Sometimes the distributed build was actually *slower* than my laptop.
**Tackle**: I learned that if the tasks are too small (like compiling a 5-line file), the time it takes to send the message over the network is longer than the work itself. I solved this by grouping work into bigger chunks so the "Work" outweighed the "Talk."
