# Third-party code

meshcore-linux compiles upstream sources directly rather than relinking
against system packages. The exact set of components below ships in every
built binary; all are MIT-licensed and reproduced under that license.

## MeshCore mesh stack

- **Project**: https://github.com/ripplebiz/MeshCore
- **License**: MIT — Copyright (c) 2025 Scott Powell / rippleradios.com
- **Used files**: `src/{Mesh,Dispatcher,Identity,Packet,Utils,MeshCore}.cpp`
  plus `src/helpers/{AdvertDataHelpers,BaseChatMesh,ClientACL,CommonCLI,
  IdentityStore,RegionMap,StaticPoolPacketManager,TransportKeyStore,
  TxtDataHelpers}.cpp`
- **Local copy lives at**: `../MeshCore/` (sibling checkout — not vendored
  into this repo; see Makefile `-I../MeshCore/src`)

## rweather/Crypto

- **Project**: https://github.com/rweather/arduinolibs (Crypto subfolder)
- **License**: MIT — Copyright (c) Rhys Weatherley
- **Provides**: AES128/256, Ed25519, SHA256/512, Curve25519, BLAKE2s

## ed25519-donna (reference C impl bundled by MeshCore)

- **Project**: https://github.com/floodyberry/ed25519-donna
- **License**: Public domain / MIT (see `MeshCore/lib/ed25519/license.txt`)

## CayenneLPP

- **Project**: https://github.com/ElectronicCats/CayenneLPP
- **License**: MIT — Copyright (c) Electronic Cats
- **Used for**: telemetry payload encoding (compiled but currently unused
  on the Linux build — telemetry handler returns empty).

## cpp-httplib

- **Project**: https://github.com/yhirose/cpp-httplib
- **License**: MIT — Copyright (c) 2024 Yuji Hirose
- **Bundled at**: `third_party/httplib.h`
- **Used for**: ConfigServer HTTP/JSON API.

## nlohmann/json

- **Project**: https://github.com/nlohmann/json
- **License**: MIT — Copyright (c) 2013-2025 Niels Lohmann
- **Bundled at**: `third_party/nlohmann_json.hpp`
- **Used for**: config file + HTTP body parsing.

## pymc_usb wire protocol

- **Project**: https://github.com/itk80/pymc_usb
- **License**: MIT — Copyright (c) itk80
- **Used as**: the modem-side firmware this service talks to. The wire
  protocol v0.7 is implemented in `src/LinuxTcpRadio.{h,cpp}`; no pymc_usb
  source ships in this repo.
