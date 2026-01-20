# MCP Unreal Blueprint Server - Objectives

## Primary Goal
Create an MCP server that interfaces with Unreal Engine's Python API to help Claude assist users in debugging Blueprint issues and migrating Blueprint logic to C++.

## Scope
**Read-only analysis only** - This project focuses on understanding and analyzing existing Blueprints, not creating or expanding them.

## Key Requirements
1. **Platform**: Unreal Engine 4.27 on Windows
2. **Blueprint Reading**: Extract and understand Blueprint structure, nodes, connections, and properties
3. **Debugging Support**: Provide detailed information about Blueprint execution flow, variable states, and potential issues
4. **Error Detection**: Identify common Blueprint problems and suggest fixes
5. **C++ Migration Support**: Analyze Blueprint logic to assist conversion to C++

## Target Use Cases
1. **Blueprint Analysis**: Read and describe Blueprint structure and logic
2. **Error Diagnosis**: Identify broken connections, missing references, or logic errors
3. **Performance Analysis**: Detect inefficient Blueprint patterns suitable for C++ migration
4. **C++ Migration**: Help convert Blueprint logic to C++ for performance improvement
5. **Codebase Understanding**: Trace Blueprint dependencies and C++ function usage

## Technical Approach
- Use Unreal's Python API via BlueprintAnalyzer plugin
- Implement MCP protocol for communication with Claude
- Focus on UE 4.27 compatibility
- Maintain read-only operations for safety and simplicity