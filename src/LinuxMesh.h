#pragma once

// LinuxMesh — minimal mesh::Mesh subclass that boots the full Dispatcher
// state machine, runs packet parsing + de-dup tables, and surfaces every
// received packet to a logger. This is the equivalent of "the repeater is
// running" without the high-level BaseChatMesh routing/ACL/contacts logic.
//
// To upgrade to a full repeater (advertise / forward / contacts / ACL):
// switch the base class to BaseChatMesh and implement its pure virtuals
// (onMessageRecv, onDiscoveredContact, processAck, …). The pieces are
// already compiled in (BaseChatMesh.cpp, ClientACL.cpp, etc.) — that's a
// follow-up because each override mirrors logic from examples/simple_
// repeater/MyMesh.cpp and total surface is ~1 kLOC.

#include <Mesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>

class LinuxMesh : public mesh::Mesh {
public:
  LinuxMesh(mesh::Radio& radio,
            mesh::MillisecondClock& ms,
            mesh::RNG& rng,
            mesh::RTCClock& rtc,
            mesh::PacketManager& mgr,
            mesh::MeshTables& tables)
    : mesh::Mesh(radio, ms, rng, rtc, mgr, tables) {}

  // Pure virtual from Dispatcher — called when a parsed Packet is ready.
  // We log + release so the manager pool stays available.
  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override {
    fprintf(stderr,
            "[mesh] packet type=%u route=%s path_len=%u payload_len=%u\n",
            (unsigned)pkt->getPayloadType(),
            pkt->isRouteDirect() ? "D" : "F",
            (unsigned)pkt->getPathByteLen(),
            (unsigned)pkt->payload_len);
    return ACTION_RELEASE;
  }
};
