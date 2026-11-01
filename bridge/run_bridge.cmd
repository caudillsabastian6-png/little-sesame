@echo off
rem Potion CoreS3 bridge launcher (watchdog round).
rem stdout/stderr are captured to files so a future bridge exit can never
rem again vanish without a record (the 2026-11-01 silent death, cause
rem never found, is the reason this wrapper exists).
cd /d %~dp0
"%~dp0venv\Scripts\python.exe" bridge.py >> bridge-stdout.log 2>> bridge-stderr.log
