# Character asset pipeline

Runtime loads:

- `KicsiK.glb` for mesh, skin weights, joints, and inverse bind matrices.
- `skeleton.ozz` for the optimized ozz runtime skeleton.
- `idle.ozz` when present. If it is missing, the client uses the ozz skeleton rest pose.

Generate the ozz files from the GLB with the host `gltf2ozz` tool:

```powershell
cmake -S D:\SDK\ozz-animation-master -B D:\SDK\ozz-animation-master\build\windows-tools `
  -G "Visual Studio 18 2026" -A x64 `
  -Dozz_build_tools=ON -Dozz_build_gltf=ON -Dozz_build_fbx=OFF `
  -Dozz_build_samples=OFF -Dozz_build_howtos=OFF -Dozz_build_tests=OFF `
  -Dozz_build_data=OFF -DBUILD_SHARED_LIBS=OFF

cmake --build D:\SDK\ozz-animation-master\build\windows-tools --config Debug --target gltf2ozz

cd D:\IxtreemeWorld\Client\assets\Character
D:\SDK\ozz-animation-master\build\windows-tools\src\animation\offline\gltf\gltf2ozz.exe `
  --file=D:\IxtreemeWorld\Client\assets\Character\KicsiK.glb `
  --config_file=D:\IxtreemeWorld\Client\assets\Character\ozz_import.json
```

The current `KicsiK.glb` contains one Mixamo animation clip. The generated runtime files are
`skeleton.ozz` and `idle.ozz`.
