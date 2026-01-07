# Ask Receiver Logic

`ask_receiver.c` is designed to be the "Detective". It doesn't start with keys; it listens for LiFi signals to find out *who* is in the room and then asks the Auth Server for permission to talk to them.

```mermaid
stateDiagram-v2
    direction TB
    
    [*] --> STATE_IDLE
    
    %% Optical Discovery
    STATE_IDLE --> STATE_CHECKING_CACHE : Recv LiFi "KEY_ID_ONLY"
    
    state STATE_CHECKING_CACHE {
        direction LR
        Cache_Lookup --> Found : ID in List
        Cache_Lookup --> Update_Needed : Not Found
    }
    
    STATE_CHECKING_CACHE --> STATE_IDLE : Found (Set Active)
    STATE_CHECKING_CACHE --> STATE_FETCHING_AUTH : Update_Needed<br/>Calls get_session_key_by_ID_fixed()
    
    %% Auth Query
    STATE_FETCHING_AUTH --> STATE_IDLE : Success<br/>(Add to List & Set Active)
    STATE_FETCHING_AUTH --> STATE_IDLE : Failure<br/>(Stay Idle / Error Msg)
    
    %% Manual Reference
    STATE_IDLE --> STATE_MANUAL_ENTRY : Shortcut 'k'
    STATE_MANUAL_ENTRY --> STATE_CHECKING_CACHE : User Enters Hex ID
```
