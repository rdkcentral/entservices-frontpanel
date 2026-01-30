# RDK EntServices FrontPanel - Architecture

## Overview

The RDK EntServices FrontPanel is a WPEFramework (Thunder) plugin that provides a comprehensive interface for controlling front panel hardware components on RDK-based devices. It acts as an abstraction layer between high-level applications and low-level device-specific front panel hardware, enabling standardized control of LEDs, displays, and other front panel indicators.

## System Architecture

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Client Applications                       │
├─────────────────────────────────────────────────────────────┤
│                  JSON-RPC Interface                         │
├─────────────────────────────────────────────────────────────┤
│              WPEFramework/Thunder Core                       │
├─────────────────────────────────────────────────────────────┤
│                FrontPanel Plugin                            │
│  ┌─────────────────────┬─────────────────────────────────┐  │
│  │   FrontPanel.cpp    │  FrontPanelImplementation.cpp   │  │
│  │   (Plugin Interface)│     (Business Logic)            │  │
│  └─────────────────────┴─────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────┤
│                    Helper Layer                             │
│  ┌─────────────────────────────────────────────────────────┐│
│  │              CFrontPanel (frontpanel.cpp)               ││
│  │                 (Hardware Abstraction)                  ││
│  └─────────────────────────────────────────────────────────┘│
├─────────────────────────────────────────────────────────────┤
│              Device Services (DS) API                       │
├─────────────────────────────────────────────────────────────┤
│                Hardware Drivers                             │
└─────────────────────────────────────────────────────────────┘
```

### Core Components

#### 1. Plugin Layer (`plugin/`)
- **FrontPanel.cpp**: Main plugin class implementing `PluginHost::IPlugin` and `PluginHost::JSONRPC`
- **FrontPanelImplementation.cpp**: Core business logic implementing `Exchange::IFrontPanel` interface
- **Module.cpp**: Thunder framework module definitions and metadata

#### 2. Helper Layer (`helpers/`)
- **frontpanel.cpp/h**: Hardware abstraction layer and device control logic
- **Utility modules**: Various helper classes for JSON-RPC, logging, IARM communication, etc.

#### 3. Interface Layer
- **JSON-RPC Interface**: REST-like API for external applications
- **Exchange Interface**: C++ interface for internal Thunder plugin communication
- **IARM Integration**: Inter-process communication with system services

### Key Architectural Patterns

#### 1. Singleton Pattern
- `CFrontPanel` implements singleton pattern for hardware resource management
- Ensures single point of access to front panel hardware

#### 2. Observer Pattern
- Event notification system for power state changes
- Multiple observers can register for front panel state updates

#### 3. Factory Pattern
- Device Services integration uses factory pattern for hardware abstraction
- Platform-specific implementations hidden behind common interface

#### 4. Command Pattern
- JSON-RPC methods encapsulated as commands
- Consistent error handling and parameter validation

### Threading and Concurrency

#### Thread Safety
- Mutex protection for shared resources
- Thread-safe singleton implementation
- Asynchronous event handling for notifications

#### Timer Management
- Blink functionality uses WPEFramework timer subsystem
- Precise timing control for LED blink patterns
- Resource cleanup on timer expiration

### Integration Points

#### 1. WPEFramework/Thunder Integration
- Plugin lifecycle management (Initialize, Deinitialize)
- Service discovery and inter-plugin communication
- Configuration management through Thunder config system

#### 2. Device Services (DS) Integration
- Hardware abstraction through DS API
- Platform-agnostic front panel control
- Device capability discovery

#### 3. Power Manager Integration
- Power state monitoring and response
- LED behavior based on system power state
- Thermal management integration

#### 4. IARM Bus Integration
- System-wide event distribution
- Inter-process communication with system services
- Hardware event propagation

### Configuration Management

#### Build-time Configuration
- CMake-based feature selection
- Platform-specific compile flags
- Optional component inclusion

#### Runtime Configuration
- JSON-based preference system
- Persistent settings storage
- Dynamic parameter updates

### Error Handling Strategy

#### Layered Error Handling
1. **Hardware Layer**: Device-specific error codes
2. **Abstraction Layer**: Standardized error mapping
3. **Plugin Layer**: JSON-RPC error responses
4. **Framework Layer**: Thunder error propagation

#### Exception Safety
- RAII principles for resource management
- Exception boundaries at API interfaces
- Graceful degradation on hardware failures

### Testing Architecture

#### Multi-level Testing
1. **L1 Tests**: Unit tests with mocked dependencies
2. **L2 Tests**: Integration tests with real hardware interfaces
3. **Mock Framework**: Comprehensive mocking of hardware and system dependencies

#### Test Isolation
- Independent test fixtures
- Mock object lifecycle management
- Predictable test state setup/teardown

### Performance Considerations

#### Resource Management
- Minimal memory footprint
- Efficient hardware state caching
- Optimized JSON parsing and generation

#### Real-time Constraints
- Sub-millisecond LED control response
- Precise blink timing accuracy
- Non-blocking API operations

### Security and Safety

#### Input Validation
- Parameter range checking
- JSON schema validation
- SQL injection prevention

#### Hardware Protection
- Safe brightness level enforcement
- Hardware capability boundary checks
- Thermal protection integration
