#pragma once
// Embedded TLS material for the MQTT uplink.
//
// SECURITY: embedding here is preferred over the SD card - a removable card
// exposes the private key to anyone with physical access, and a swapped card
// could inject a rogue CA. Embedded material always takes precedence; the SD
// files (/mqtt_ca.pem, /mqtt_cert.pem, /mqtt_key.pem) are only consulted for
// entries left empty below.
//
// Usage: paste the PEM blocks between the R"PEM( )PEM" markers, rebuild and
// flash (cable or OTA). Example:
//
//   static const char MQTT_CA_CERT[] = R"PEM(
//   -----BEGIN CERTIFICATE-----
//   MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
//   ...
//   -----END CERTIFICATE-----
//   )PEM";
//
// NOTE: for production hardware, enable ESP32-S3 flash encryption so the
// embedded key cannot be read out of the flash chip either. That is an
// irreversible eFuse operation - do it deliberately on production units.

// Broker CA certificate (server verification)
static const char MQTT_CA_CERT[] = "";

// Client certificate + private key (only for mutual-TLS brokers)
static const char MQTT_CLIENT_CERT[] = "";
static const char MQTT_CLIENT_KEY[] = "";
