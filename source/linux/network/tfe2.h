#pragma once

bool tfeReceiveOnePacket(const uint8_t * mac, const int size, BYTE * buffer, int & len);
void tfeTransmitOnePacket(const BYTE * buffer, const int len);
