import subprocess
import threading
import sys
import json
import time

def stream_reader(stream, prefix):
    # Read raw bytes
    while True:
        data = stream.read(1024) # Read in 1KB chunks
        if not data:
            break
        print(f"[{prefix}] {data.decode('utf-8', errors='replace')}", end='', flush=True)


def start_lsp_server(command):
    # Start the LSP server process
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0  # Unbuffered
    )

    # Start threads to monitor stdout and stderr
    threading.Thread(target=stream_reader, args=(process.stdout, "STDOUT"), daemon=True).start(),
    threading.Thread(target=stream_reader, args=(process.stderr, "STDERR"), daemon=True).start()

    # Define the LSP Initialize message
    # Note: LSP requires a Content-Length header followed by two \r\n
    msg = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {"processId": None, "rootUri": None, "capabilities": {}}
    }
    content = json.dumps(msg)
    header = f"Content-Length: {len(content)}\r\n\r\n{content}"

    # Send the message
    print("[SYSTEM] Sending initialize message...")
    process.stdin.write(header.encode('utf-8'))
    process.stdin.flush()

    # Set the duration to stay alive (e.g., 5 seconds)
    timeout = 5 
    start_time = time.time()
    
    print(f"[SYSTEM] Monitoring server for {timeout} seconds...")
    
    try:
        while time.time() - start_time < timeout:
            if process.poll() is not None:
                # The process died before the timer finished
                break
            time.sleep(0.5)
        else:
            print("[SYSTEM] Timeout reached. Shutting down server...")
    except KeyboardInterrupt:
        print("\n[SYSTEM] Terminating server...")
        process.terminate()
    finally:
        # Gracefully terminate the process if it's still running
        if process.poll() is None:
            process.terminate()
            # Optional: give it a moment to clean up before final check
            process.wait(timeout=2)
            
        return_code = process.poll()
        print(f"[SYSTEM] Server exited with return code: {return_code}")


if __name__ == "__main__":
    # Replace with your actual LSP server command (e.g., ["pyright-langserver", "--stdio"])
    server_command = ['./build/src/toy_lsp']
    start_lsp_server(server_command)
