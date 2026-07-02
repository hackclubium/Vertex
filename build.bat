@echo off
setlocal
echo [Vertex] Configuring...
cmake -B build -A x64
if %errorlevel% neq 0 (
  echo [Vertex] Configure failed.
  pause
  exit /b 1
)

echo [Vertex] Building...
cmake --build build --config Release
if %errorlevel% neq 0 (
  echo [Vertex] Build failed.
  pause
  exit /b 1
)

echo.
echo [Vertex] Done! Run: build\Release\Vertex.exe
