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

    # Send LSP Initialize message
    init_msg = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "initialize",
        "params": {"processId": None, "rootUri": None, "capabilities": {}}
    }
    init_content = json.dumps(init_msg)
    init_header = f"Content-Length: {len(init_content)}\r\n\r\n{init_content}"
    
    # Send document open message
    doc_open_msg = {
        "jsonrpc": "2.0",
        "method": "textDocument/didOpen",
        "params": {
            "textDocument": {
                "uri": "file:///test.toy",
                "languageId": "toylang",
                "version": 1,
                "text": "hello world hello foo bar"
            }
        }
    }
    doc_open_content = json.dumps(doc_open_msg)
    doc_open_header = f"Content-Length: {len(doc_open_content)}\r\n\r\n{doc_open_content}"
    
    # Send completion request
    completion_msg = {
        "jsonrpc": "2.0",
        "id": 2,
        "method": "textDocument/completion",
        "params": {
            "textDocument": {"uri": "file:///test.toy"},
            "position": {"line": 0, "character": 0}
        }
    }
    completion_content = json.dumps(completion_msg)
    completion_header = f"Content-Length: {len(completion_content)}\r\n\r\n{completion_content}"
    
    # Send shutdown
    shutdown_msg = {
        "jsonrpc": "2.0",
        "id": 3,
        "method": "shutdown",
        "params": None
    }
    shutdown_content = json.dumps(shutdown_msg)
    shutdown_header = f"Content-Length: {len(shutdown_content)}\r\n\r\n{shutdown_content}"
    
    # Send exit
    exit_msg = {
        "jsonrpc": "2.0",
        "method": "exit",
        "params": None
    }
    exit_content = json.dumps(exit_msg)
    exit_header = f"Content-Length: {len(exit_content)}\r\n\r\n{exit_content}"

    # Send all messages
    print("[SYSTEM] Sending LSP messages...")
    for header in [init_header, doc_open_header, completion_header, shutdown_header, exit_header]:
        process.stdin.write(header.encode('utf-8'))
        process.stdin.flush()
        time.sleep(0.1)  # Small delay between messages

    # Set the duration to stay alive
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
            process.wait(timeout=2)
            
        return_code = process.poll()
        print(f"[SYSTEM] Server exited with return code: {return_code}")


if __name__ == "__main__":
    server_command = ['./build/src/toy_lsp']
    start_lsp_server(server_command)