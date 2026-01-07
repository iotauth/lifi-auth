# Keys Receiver Logic

`keys_receiver.c` is the "Manager". It fetches fresh keys from the Auth Server at startup and pushes them to the Pico (Sender) so the Pico has valid credentials to broadcast.

```mermaid
stateDiagram-v2
    direction TB
    
    [*] --> FETCH_INITIAL
    
    FETCH_INITIAL --> STATE_IDLE : Success (Keys Loaded)<br/>Calls get_session_key()
    FETCH_INITIAL --> STATE_IDLE : Fail (Empty List)
    
    %% Manual Push
    STATE_IDLE --> STATE_PUSHING_KEY : Shortcut '1'<br/>Calls get_session_key()
    STATE_PUSHING_KEY --> STATE_IDLE : Key Sent over UART
    
    %% Force Rotation
    STATE_IDLE --> STATE_FETCHING_NEW : Shortcut 'f'<br/>Calls get_session_key()
    
    STATE_FETCHING_NEW --> STATE_PUSHING_KEY : Success (New List)
    STATE_FETCHING_NEW --> STATE_FETCH_FAIL : Fail
    STATE_FETCH_FAIL --> STATE_IDLE : Keep Old Keys
    
    %% Local Rotation
    STATE_IDLE --> STATE_IDLE : Shortcut 'n' (Rotate Local Index)
```
