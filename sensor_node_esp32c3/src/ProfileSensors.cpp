#include "ProfileSensors.h"

#include <Wire.h>

namespace {

// Builds a DriverConfig for a profile sensor. All Profile 0 sensors are
// declared here so that the roster is auditable in one place.
DriverConfig makeConfig(uint8_t driverType, uint16_t sensorId,
                        uint32_t periodMs) {
  DriverConfig c{};
  c.driverType = driverType;
  c.sensorId = sensorId;
  c.periodMs = periodMs;
  return c;
}

}  // namespace

bool ProfileSensors::begin(ProfileConfig::Profile profile,
                           SensorDriverRegistry& drivers,
                           SensorRegistry& registry) {
  registeredCount_ = 0;
  expectedCount_ = 0;
  switch (profile) {
    case ProfileConfig::Profile::IslandSea:
      return registerIslandSea(drivers, registry);
    case ProfileConfig::Profile::TropicalForest:
      return registerTropicalForest(drivers, registry);
    case ProfileConfig::Profile::VolcanicMountain:
      return registerVolcanicMountain(drivers, registry);
    case ProfileConfig::Profile::SubZeroSnow:
      return registerSubZeroSnow(drivers, registry);
  }
  return false;
}

bool ProfileSensors::registerIslandSea(SensorDriverRegistry& drivers, SensorRegistry& registry) {
  // Profile 0 roster (6 sensors, per userdecisions.txt Q3.5):
  //   0x0100 water_temp      DS18B20            OneWire on GPIO3
  //   0x0101 water_ec        Atlas EZO-EC       I2C 0x64
  //   0x0102 water_ph        Atlas EZO-pH       I2C 0x63
  //   0x0103 water_current   current-meter      UART (parser deferred)
  //   0x0104 water_wave      pressure transducer ADC
  //   0x0105 water_turbidity SEN0189            ADC
  //
  // The UART current-meter is registered with a placeholder driver: it never
  // fabricates a value, it returns QUALITY_STALE until a hardware-specific
  // parser is provided. This is intentional and documented in README.

  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_ISLAND_SEA;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;

  // Wire bus shared by all I2C profile sensors.
  Wire.begin(ProfileConfig::PROFILE0_I2C_SDA_PIN,
             ProfileConfig::PROFILE0_I2C_SCL_PIN);
  Wire.setClock(ProfileConfig::PROFILE0_I2C_HZ);

  expectedCount_ = 6;

  // 0x0100 — DS18B20 water temperature.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ONEWIRE_TEMP,
                                base + 0, period);
    c.pinSda = ProfileConfig::PROFILE0_ONEWIRE_PIN;
    c.registerAddr = 0;  // water temperature
    c.channel = 0;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::OneWire);
    if (drivers.add(c, registry)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0101 — Atlas EZO-EC salinity/conductivity.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ATLAS_EZO,
                                base + 1, period);
    c.pinSda = ProfileConfig::PROFILE0_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE0_I2C_SCL_PIN;
    c.i2cAddr = 0x64;  // Atlas EZO-EC default
    c.registerAddr = 0;  // EC
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (drivers.add(c, registry)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0102 — Atlas EZO-pH.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ATLAS_EZO,
                                base + 2, period);
    c.pinSda = ProfileConfig::PROFILE0_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE0_I2C_SCL_PIN;
    c.i2cAddr = 0x63;  // Atlas EZO-pH default
    c.registerAddr = 1;  // pH
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (drivers.add(c, registry)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0103 — Current meter (generic UART). Parser NOT VERIFIED.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,
                                base + 3, period);
    c.dataWidth = 4;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (drivers.add(c, registry)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0104 — Wave pressure transducer (generic ADC).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,
                                base + 4, period);
    c.pinSda = ProfileConfig::PROFILE0_WAVE_ADC_PIN;  // GPIO3 (ADC1_CH3)
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (drivers.add(c, registry)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0105 — SEN0189 turbidity (generic ADC).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,
                                base + 5, period);
    c.pinSda = ProfileConfig::PROFILE0_TURBIDITY_ADC_PIN;  // GPIO4 (ADC1_CH4)
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (drivers.add(c, registry)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  Serial.printf("PROFILE: island_sea roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}
bool ProfileSensors::registerTropicalForest(SensorDriverRegistry& drivers, SensorRegistry& registry) {
  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_TROPICAL_FOREST;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;
  Wire.begin(ProfileConfig::PROFILE1_I2C_SDA_PIN, ProfileConfig::PROFILE1_I2C_SCL_PIN);
  Wire.setClock(ProfileConfig::PROFILE1_I2C_HZ);
  expectedCount_ = 12;
  for (uint8_t depth=0; depth<3; ++depth) { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,base+depth,period); c.dataWidth=4; c.channel=depth; c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART); if(drivers.add(c,registry)) ++registeredCount_; }
  for (uint8_t depth=0; depth<3; ++depth) { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_ONEWIRE_TEMP,base+3+depth,period); c.pinSda=ProfileConfig::PROFILE1_ONEWIRE_PIN; c.registerAddr=1; c.channel=depth; c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::OneWire); if(drivers.add(c,registry)) ++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_ATLAS_EZO,base+6,period); c.pinSda=ProfileConfig::PROFILE1_I2C_SDA_PIN;c.pinScl=ProfileConfig::PROFILE1_I2C_SCL_PIN;c.i2cAddr=0x63;c.registerAddr=1;c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+7,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x62;c.dataWidth=2;c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,base+8,period);c.pinSda=ProfileConfig::PROFILE1_CH4_ADC_PIN;c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_BME280,base+9,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x76;c.registerAddr=0;c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+10,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=ProfileConfig::RAIN_GAUGE_I2C_ADDRESS;c.registerAddr=0;c.dataWidth=2;c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,base+11,period);c.dataWidth=4;c.registerAddr=1;c.interfaceType=static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);if(drivers.add(c,registry))++registeredCount_; }
  Serial.printf("PROFILE: tropical_forest roster %u/%u sensors\\n",(unsigned)registeredCount_,(unsigned)expectedCount_); return registeredCount_==expectedCount_;
}

bool ProfileSensors::registerVolcanicMountain(SensorDriverRegistry& drivers, SensorRegistry& registry) {
  const uint16_t base=ProfileConfig::SENSOR_ID_BASE_VOLCANIC_MOUNTAIN; const uint32_t period=SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;
  Wire.begin(8,9); Wire.setClock(ProfileConfig::PROFILE2_I2C_HZ); expectedCount_=8;
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,base+0,period);c.pinSda=4;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::Adc;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+1,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x62;c.dataWidth=2;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,base+2,period);c.dataWidth=4;c.registerAddr=2;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::UART;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_BME280,base+3,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x76;c.registerAddr=0;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+4,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=ProfileConfig::RAIN_GAUGE_I2C_ADDRESS;c.dataWidth=2;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,base+5,period);c.pinSda=3;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::Adc;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_PULSE_COUNTER,base+6,period);c.pinSda=11;c.registerAddr=1;c.dataWidth=1;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::Pulse;if(drivers.add(c,registry))++registeredCount_; }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+7,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x1D;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::SPI;if(drivers.add(c,registry))++registeredCount_; }
  return registeredCount_==expectedCount_;
}

bool ProfileSensors::registerSubZeroSnow(SensorDriverRegistry& drivers, SensorRegistry& registry) {
  const uint16_t base=ProfileConfig::SENSOR_ID_BASE_SUB_ZERO_SNOW; const uint32_t period=SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;
  Wire.begin(8,9); Wire.setClock(ProfileConfig::PROFILE3_I2C_HZ); expectedCount_=9;
  auto add=[&](DriverConfig c){if(drivers.add(c,registry))++registeredCount_;};
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+0,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x20;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::SPI;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_ONEWIRE_TEMP,base+1,period);c.pinSda=3;c.registerAddr=1;c.channel=0;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::OneWire;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,base+2,period);c.dataWidth=4;c.registerAddr=3;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::UART;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_BME280,base+3,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x76;c.registerAddr=0;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+4,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x10;c.dataWidth=2;c.registerAddr=2;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,base+5,period);c.dataWidth=4;c.registerAddr=1;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::UART;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+6,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x70;c.dataWidth=2;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+7,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x73;c.dataWidth=2;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::I2C;add(c); }
  { DriverConfig c=makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,base+8,period);c.pinSda=8;c.pinScl=9;c.i2cAddr=0x1D;c.interfaceType=(uint8_t)ProfileConfig::InterfaceKind::SPI;add(c); }
  return registeredCount_==expectedCount_;
}
