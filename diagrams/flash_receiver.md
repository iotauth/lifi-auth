# Flash Receiver Application Logic

`flash_receiver.c` is the primary receiver application. It combines file transfer, secure chat, and authentication verification into one UI-driven tool.

## Core Responsibilities
1.  **Secure Chat/Command**: Receives encrypted text messages.
2.  **File Transfer**: Receives and decompresses files (`.txt`, `.jpg`, etc.).
3.  **Authentication**: Verifies the Sender (Pico) via HMAC Challenge-Response.
4.  **Key Management**: Requests new keys from Auth and pushes them to the Sender.

## Application Flowchart

```mermaid
flowchart TD
    %% Initialization Phase
    Start([Start]) --> Init_SST[Init SST & Serial]
    Init_SST --> Init_UI[Init UI ncurses]
    Init_UI --> MainLoop{Main Loop}

    %% User Input Logic
    MainLoop -->|Key Code| Check_Input{Check Key}
    
    Check_Input -->|'q'| Exit([Exit])
    
    Check_Input -->|'n'| Cmd_NewKey[Cmd: New Key]
    Cmd_NewKey --> SetState_Yes[State waiting: YES]
    SetState_Yes --> MainLoop
    
    Check_Input -->|'2'| Cmd_Verify[Cmd: Verify]
    Cmd_Verify --> SendChallenge[Send Challenge]
    SendChallenge --> SetState_HMAC[State waiting: HMAC]
    SetState_HMAC --> MainLoop

    %% UART Input Path
    MainLoop -->|UART Byte Recv| Update_SM[Update State Machine]
    Update_SM -->|Preamble Complete| Check_MsgType{Msg Type?}

    %% Message Type Handling
    Check_MsgType -->|MSG_TYPE_ENCRYPTED| Decrypt_Msg[Decrypt Payload]
    Check_MsgType -->|MSG_TYPE_FILE| Process_File[Process File Chunk]
    
    %% Decrypted Logic (Chat/Commands)
    Decrypt_Msg -->|User Text| Show_Msg[Display Message]
    Decrypt_Msg -->|'ACK'| Handle_ACK{State == WAITING_ACK?}
    Decrypt_Msg -->|'HMAC:...'| Handle_HMAC{State == WAITING_HMAC?}
    
    %% HMAC Verification Logic
    Handle_HMAC -->|Yes| Verify_Sign[Verify HMAC Signature]
    Verify_Sign -->|Match| Success[✅ Identity Confirm]
    Success --> Print_Success[Print 'HMAC VERIFIED' to UI]
    Verify_Sign -->|Mismatch| Fail[❌ Verification Fail]
    Fail --> Print_Fail[Print 'Verification Failed' to UI]
    
    Handle_ACK -->|Yes| Finish_Key[Complete Key Update]
    Finish_Key --> Print_ACK[Print 'ACK Received' & Update UI]
    
    %% Loop Back
    Print_Success --> Reset_State[Reset State to IDLE]
    Print_Fail --> Reset_State
    Print_ACK --> Reset_State
    Reset_State --> MainLoop
    Show_Msg --> MainLoop
    Process_File --> MainLoop
```
