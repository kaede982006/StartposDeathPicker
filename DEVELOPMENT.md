cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
cmake --build build --target clean
Remove-Item -Recurse -Force build
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
