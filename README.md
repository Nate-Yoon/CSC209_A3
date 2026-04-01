# Survive the Internet (CSC209 A3)

A multiplayer party game inspired by **Survive the Internet**.

This project is a socket-based client-server game written in C for CSC209.

## Overview

Players join the same lobby, answer prompts, rewrite another player's response into a funny headline, and then vote on the funniest result. The server manages all game state, round flow, scoring, and results, while each client provides a terminal-based interface for one player.

This version is designed for **3 to 5 players**.

## Requirements

- GCC with C11 support
- `make`
- A Unix-like environment such as Linux or macOS

## Build

Compile both programs with:

```bash
make
```

This creates two executables:

- `server`
- `client`

To remove compiled files:

```bash
make clean
```

## Running the Game

### 1. Start the server

Run the server in one terminal:

```bash
./server
```

By default, the project uses port `4242`.

You can also provide a custom port:

```bash
./server 5000
```

### 2. Start the clients

Open a separate terminal for each player and connect each client to the server:

```bash
./client localhost
```

Or, if using a custom port:

```bash
./client localhost 5000
```

If playing across different machines, replace `localhost` with the server machine's IP address or hostname.

## How to Start a Match

1. Launch the server.
2. Launch **3 to 5 clients**.
3. Each client enters a unique alphanumeric username when prompted.
4. After joining, each player types:

```text
READY
```

5. Once enough players have joined and everyone is ready, the game begins.

## Gameplay

A game proceeds in rounds:

1. **Question phase**  
   Each player receives a prompt and submits an answer.

2. **Headline / rewrite phase**  
   Each player is shown another player's content and writes a funny headline or twist for it.

3. **Voting phase**  
   Players vote for the funniest result.

4. **Results phase**  
   The server reveals the round outcome, updates scores, and continues until the game ends.


## What the game is

The fun of the game comes from seeing ordinary answers turned into something more dramatic, ridiculous, or embarrassing. A response that looks harmless at first can become much funnier once it is shown in a different way later in the round.

The goal is to be the funniest player in the group by giving answers and rewrites that get the best reactions from everyone else.
