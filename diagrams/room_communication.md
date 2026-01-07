# Smart Room Communication Flow

This diagram illustrates how a LiFi-enabled device (Sender/Tag) enters a room, communicates its identity to the Detector (Receiver), and how the Detector verifies this identity with the central Authentication Service (SST).

```mermaid
sequenceDiagram
    autonumber
    
    box rgb(255, 248, 240) Smart Room (Physical Space)
        participant Pico as Sender (LiFi Tag)
        participant Pi4 as Receiver (Detector)
    end
    
    box rgb(235, 245, 255) Backend Network
        participant SST as Auth Server (SST)
    end

    Note over Pico: **Device Enters Room**<br/>Broadcasts Identity via Light
    
    loop Optical Broadcast
        Pico->>Pi4: **MSG_TYPE_KEY_ID_ONLY**<br/>Payload: [Key ID (8 bytes)]
    end
    
    activate Pi4
    Pi4->>Pi4: **Receive Key ID**<br/>Parse & Check Integrity (CRC)
    
    Note over Pi4: **Discovery Phase**<br/>"I see Device ID: 0x12..AB"
    
    Pi4->>Pi4: Check Local Cache<br/>(Is Key loaded?)
    
    alt Key Not Found Locally
        Note over Pi4: **Roaming Check**
        Pi4->>SST: **get_session_key_by_ID**(Key ID)
        activate SST
        
        Note right of Pi4: Secure TCP/IP Connection<br/>(Mutual TLS/SST Crypto)
        
        SST->>SST: Database Lookup
        
        alt Valid Device
            SST-->>Pi4: **Return Session Key**<br/>(Cipher Key + MAC Key)
        else Unknown/Revoked
            SST-->>Pi4: Error: Key Not Found
        end
        deactivate SST
    else Key Found Locally
        Note over Pi4: Using Cached Credentials
    end
    
    opt If Key Available
        Pi4->>Pi4: **Load Key** into Active Session
        
        Note over Pi4: **Verification Phase**<br/>(Optional but Recommended)
        
        Pi4->>Pico: **MSG_TYPE_CHALLENGE**
        activate Pico
        Pico-->>Pi4: **MSG_TYPE_ENCRYPTED(HMAC)**
        deactivate Pico
        
        Pi4->>Pi4: **Authenticate**
        Note over Pi4: "Device 0x12..AB is present and valid!"
    end
    deactivate Pi4
```
