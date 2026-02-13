# No Automatic Builds

**CRITICAL RULE**: Never automatically run build commands and try to kill it everytime.

## Forbidden Commands
- `idf.py build`
- `idf.py flash`
- `idf.py monitor`
- `python ./scripts/release.py`
- Any compilation/linking commands

## Why?
- User manages build process manually
- Builds are time-consuming
- User has specific build workflow

## What to Do Instead
When user needs to build, provide the command:
```bash
source /Users/sohilpatel/esp/v5.5/esp-idf/export.sh
idf.py build
```

But **NEVER execute it automatically and dont try to kill it everytime**.
