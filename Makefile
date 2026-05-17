# Meshcore-Linux — Phase D build.
#
#   make           # build ./meshcore-linux
#   make run       # build and run against ./config/config.example.json
#   make clean
#
# Compiles upstream MeshCore core (Mesh/Dispatcher/Identity/Packet/Utils)
# directly from ../MeshCore/src plus the rweather/Crypto and ed25519 libs
# from the MeshCore PlatformIO libdeps cache. No vendoring: meshcore-linux
# references the upstream tree in place.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -Wno-reorder-ctor -Wno-unused-result -Wno-sign-compare \
            -include cstddef -include cstdint -include cstring -include cstdio \
            -include shims/Arduino.h -include shims/FS.h \
            -Ishims \
            -Ithird_party \
            -I../MeshCore/src \
            -I../MeshCore/lib/ed25519 \
            -I../MeshCore/.pio/libdeps/Heltec_v3_repeater/Crypto
LDFLAGS  ?= -pthread

# ── Our own source ───────────────────────────────────────────────────
LINUX_SRC := \
    src/main.cpp \
    src/LinuxTcpRadio.cpp \
    src/LinuxRepeaterMesh.cpp \
    src/ConfigServer.cpp \
    shims/RNGStub.cpp

# ── MeshCore upstream core (compiled in-place, no vendoring) ─────────
MC := ../MeshCore/src
MESHCORE_SRC := \
    $(MC)/Utils.cpp \
    $(MC)/Mesh.cpp \
    $(MC)/Dispatcher.cpp \
    $(MC)/Identity.cpp \
    $(MC)/Packet.cpp \
    $(MC)/helpers/AdvertDataHelpers.cpp \
    $(MC)/helpers/BaseChatMesh.cpp \
    $(MC)/helpers/ClientACL.cpp \
    $(MC)/helpers/CommonCLI.cpp \
    $(MC)/helpers/IdentityStore.cpp \
    $(MC)/helpers/RegionMap.cpp \
    $(MC)/helpers/StaticPoolPacketManager.cpp \
    $(MC)/helpers/TransportKeyStore.cpp \
    $(MC)/helpers/TxtDataHelpers.cpp

# CayenneLPP — used by SensorManager (we stub it out but the compile path
# still pulls in the header from MeshCore code).
CLPP := ../MeshCore/.pio/libdeps/Heltec_v3_repeater/CayenneLPP/src
CLPP_SRC := $(CLPP)/CayenneLPP.cpp $(CLPP)/CayenneLPPPolyline.cpp

# ── rweather/Crypto — vendored via PlatformIO libdeps cache ──────────
CRYPTO := ../MeshCore/.pio/libdeps/Heltec_v3_repeater/Crypto
CRYPTO_SRC := \
    $(CRYPTO)/AES128.cpp $(CRYPTO)/AES256.cpp $(CRYPTO)/AESCommon.cpp \
    $(CRYPTO)/Ed25519.cpp $(CRYPTO)/SHA256.cpp $(CRYPTO)/SHA512.cpp \
    $(CRYPTO)/Curve25519.cpp $(CRYPTO)/BigNumberUtil.cpp \
    $(CRYPTO)/Hash.cpp $(CRYPTO)/Crypto.cpp \
    $(CRYPTO)/BLAKE2s.cpp \
    $(CRYPTO)/Cipher.cpp $(CRYPTO)/BlockCipher.cpp \
    $(CRYPTO)/NoiseSource.cpp

# ── ed25519 reference C impl from MeshCore/lib/ ──────────────────────
ED := ../MeshCore/lib/ed25519
ED_SRC := \
    $(ED)/add_scalar.c $(ED)/fe.c $(ED)/ge.c $(ED)/key_exchange.c \
    $(ED)/keypair.c $(ED)/sc.c $(ED)/seed.c $(ED)/sha512.c \
    $(ED)/sign.c $(ED)/verify.c

BIN := meshcore-linux

all: $(BIN)

$(BIN): $(LINUX_SRC) $(MESHCORE_SRC) $(CRYPTO_SRC) $(ED_SRC) $(CLPP_SRC)
	$(CXX) $(CXXFLAGS) $(CXXFLAGS_EXTRA) -I$(CLPP) $^ -o $@ $(LDFLAGS)

run: $(BIN)
	./$(BIN) config/config.example.json

clean:
	rm -f $(BIN)

.PHONY: all run clean
