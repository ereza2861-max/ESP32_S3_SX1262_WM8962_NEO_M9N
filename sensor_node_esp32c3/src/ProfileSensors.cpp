#include "ProfileSensors.h"
#include "Config.h"

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

bool addDriver(SensorDriverRegistry& drivers, SensorRegistry& registry,
               const DriverConfig& config) {
  if (drivers.add(config, registry)) return true;
  Serial.printf("PROFILE: sensor 0x%04X registration failed (driver=%u)\n",
                static_cast<unsigned>(config.sensorId),
                static_cast<unsigned>(config.driverType));
  return false;
}

}  // namespace

bool ProfileSensors::begin(ProfileConfig::Profile profile,
                           SensorDriverRegistry& drivers,
                           SensorRegistry& registry) {
  registeredCount_ = 0;
  expectedCount_ = ProfileConfig::expectedSensorCount(profile);

  // I2C ownership is centralized here; individual drivers never call Wire.begin().
  Wire.begin(ProfileConfig::PROFILE0_I2C_SDA_PIN, ProfileConfig::PROFILE0_I2C_SCL_PIN);
  Wire.setClock(ProfileConfig::PROFILE0_I2C_HZ);

  switch (profile) {
    case ProfileConfig::Profile::IslandSea:
      return registerIslandSea(drivers, registry);
    case ProfileConfig::Profile::TropicalForest:
      return registerTropicalForest(drivers, registry);
    case ProfileConfig::Profile::VolcanicMountain:
      return registerVolcanicMountain(drivers, registry);
    case ProfileConfig::Profile::SubZeroSnow:
      return registerSubZeroSnow(drivers, registry);
    case ProfileConfig::Profile::Desert:
      return registerDesert(drivers, registry);
    case ProfileConfig::Profile::MineTunnel:
      return registerMineTunnel(drivers, registry);
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

  expectedCount_ = ProfileConfig::expectedSensorCount(ProfileConfig::Profile::IslandSea);

  // 0x0100 — DS18B20 water temperature.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ONEWIRE_TEMP,
                                base + 0, period);
    c.pinSda = ProfileConfig::PROFILE0_ONEWIRE_PIN;
    c.registerAddr = 0;  // water temperature
    c.channel = 0;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::OneWire);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0101 — Atlas EZO-EC salinity/conductivity.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ATLAS_EZO,
                                base + 1, period);
    c.pinSda = ProfileConfig::PROFILE0_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE0_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::ATLAS_EZO_EC_I2C_ADDR;
    c.registerAddr = 0;  // EC
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0102 — Atlas EZO-pH.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ATLAS_EZO,
                                base + 2, period);
    c.pinSda = ProfileConfig::PROFILE0_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE0_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::ATLAS_EZO_PH_I2C_ADDR;
    c.registerAddr = 1;  // pH
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0103 — Current meter (generic UART). Parser NOT VERIFIED.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,
                                base + 3, period);
    c.dataWidth = 4;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0104 — Wave pressure transducer (generic ADC).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,
                                base + 4, period);
    c.pinSda = ProfileConfig::PROFILE0_WAVE_ADC_PIN;  // GPIO3 (ADC1_CH3)
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  // 0x0105 — SEN0189 turbidity (generic ADC).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,
                                base + 5, period);
    c.pinSda = ProfileConfig::PROFILE0_TURBIDITY_ADC_PIN;  // GPIO4 (ADC1_CH4)
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
    else Serial.printf("PROFILE: island_sea sensor 0x%04X unavailable\n", c.sensorId);
  }

  Serial.printf("PROFILE: island_sea roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}
bool ProfileSensors::registerTropicalForest(
    SensorDriverRegistry& drivers, SensorRegistry& registry) {
  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_TROPICAL_FOREST;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;


  expectedCount_ =
      ProfileConfig::expectedSensorCount(ProfileConfig::Profile::TropicalForest);

  for (uint8_t depth = 0; depth < 3; ++depth) {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,
                                base + depth, period);
    c.dataWidth = 4;
    c.channel = depth;
    c.interfaceType =
        static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  for (uint8_t depth = 0; depth < 3; ++depth) {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_ONEWIRE_TEMP,
                                base + 3 + depth, period);
    c.pinSda = ProfileConfig::PROFILE1_ONEWIRE_PIN;
    c.registerAddr = 1;
    c.channel = depth;
    c.interfaceType =
        static_cast<uint8_t>(ProfileConfig::InterfaceKind::OneWire);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_ATLAS_EZO, base + 6, period);
    c.pinSda = ProfileConfig::PROFILE1_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE1_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::ATLAS_EZO_PH_I2C_ADDR;
    c.registerAddr = 1;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 7, period);
    c.pinSda = ProfileConfig::PROFILE1_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE1_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::SCD4X_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC, base + 8, period);
    c.pinSda = ProfileConfig::PROFILE1_CH4_ADC_PIN;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_BME280, base + 9, period);
    c.pinSda = ProfileConfig::PROFILE1_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE1_I2C_SCL_PIN;
    c.i2cAddr = 0x76;
    c.registerAddr = 0;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 10, period);
    c.pinSda = ProfileConfig::PROFILE1_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE1_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::RAIN_GAUGE_I2C_ADDRESS;
    c.registerAddr = 0;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART, base + 11, period);
    c.dataWidth = 4;
    c.registerAddr = 1;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  Serial.printf("PROFILE: tropical_forest roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}

bool ProfileSensors::registerVolcanicMountain(
    SensorDriverRegistry& drivers, SensorRegistry& registry) {
  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_VOLCANIC_MOUNTAIN;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;


  expectedCount_ =
      ProfileConfig::expectedSensorCount(ProfileConfig::Profile::VolcanicMountain);

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC, base + 0, period);
    c.pinSda = ProfileConfig::PROFILE2_H2S_ADC_PIN;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 1, period);
    c.pinSda = ProfileConfig::PROFILE2_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE2_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::SCD4X_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART, base + 2, period);
    c.dataWidth = 4;
    c.registerAddr = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_BME280, base + 3, period);
    c.pinSda = ProfileConfig::PROFILE2_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE2_I2C_SCL_PIN;
    c.i2cAddr = 0x76;
    c.registerAddr = 0;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 4, period);
    c.pinSda = ProfileConfig::PROFILE2_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE2_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::RAIN_GAUGE_I2C_ADDRESS;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC, base + 5, period);
    c.pinSda = ProfileConfig::PROFILE2_WIND_VANE_ADC_PIN;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_PULSE_COUNTER, base + 6, period);
    c.pinSda = ProfileConfig::PROFILE2_WIND_PULSE_PIN;
    c.registerAddr = 1;
    c.dataWidth = 1;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Pulse);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 7, period);
    c.pinSda = ProfileConfig::PROFILE2_ADXL355_CS_PIN;
    c.pinScl = ProfileConfig::RFID_SCK_PIN;
    c.i2cAddr = 0x1D;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::SPI);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  Serial.printf("PROFILE: volcanic_mountain roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}

bool ProfileSensors::registerSubZeroSnow(
    SensorDriverRegistry& drivers, SensorRegistry& registry) {
  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_SUB_ZERO_SNOW;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;


  expectedCount_ =
      ProfileConfig::expectedSensorCount(ProfileConfig::Profile::SubZeroSnow);

  auto add = [&](const DriverConfig& c) {
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  };

  // MAX31865 remains a placeholder because the repository has no generic SPI
  // register driver. Keep the stable descriptor but do not fabricate data.
  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 0, period);
    c.pinSda = ProfileConfig::PROFILE3_MAX31865_CS_PIN;
    c.pinScl = ProfileConfig::RFID_SCK_PIN;
    c.i2cAddr = 0x20;  // placeholder retained by the source-level roster
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::SPI);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_ONEWIRE_TEMP, base + 1, period);
    c.pinSda = ProfileConfig::PROFILE3_ONEWIRE_PIN;
    c.registerAddr = 1;
    c.channel = 0;
    c.interfaceType =
        static_cast<uint8_t>(ProfileConfig::InterfaceKind::OneWire);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART, base + 2, period);
    c.dataWidth = 4;
    c.registerAddr = 3;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_BME280, base + 3, period);
    c.pinSda = ProfileConfig::PROFILE3_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE3_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE3_BME280_I2C_ADDR;
    c.registerAddr = 0;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 4, period);
    c.pinSda = ProfileConfig::PROFILE3_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE3_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE3_VEML6075_I2C_ADDR;
    c.dataWidth = 2;
    c.registerAddr = ProfileConfig::PROFILE3_VEML6075_REG_UVA;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART, base + 5, period);
    c.dataWidth = 4;
    c.registerAddr = 1;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 6, period);
    c.pinSda = ProfileConfig::PROFILE3_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE3_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE3_SNOW_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 7, period);
    c.pinSda = ProfileConfig::PROFILE3_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE3_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE3_O2_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    add(c);
  }

  {
    DriverConfig c =
        makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C, base + 8, period);
    c.pinSda = ProfileConfig::PROFILE3_ADXL355_CS_PIN;
    c.pinScl = ProfileConfig::RFID_SCK_PIN;
    c.i2cAddr = 0x1D;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::SPI);
    add(c);
  }

  Serial.printf("PROFILE: sub_zero_snow roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}

bool ProfileSensors::registerDesert(SensorDriverRegistry& drivers,
                                    SensorRegistry& registry) {
  // Profile 4 roster (8 sensors):
  //   0x0500 air_temp          BME280        I2C 0x76
  //   0x0501 humidity          BME280        I2C 0x76 (shared)
  //   0x0502 pressure          BME280        I2C 0x76 (shared)
  //   0x0503 sand_temp         generic ADC   GPIO4
  //   0x0504 soil_moisture     generic ADC   GPIO3 via mux Y1
  //   0x0505 solar_uv          VEML6075      I2C 0x10
  //   0x0506 dust_visibility   dust UART     UART1 (placeholder)
  //   0x0507 wind_pulse        pulse counter GPIO11
  //
  // Pin overlap: GPIO3/GPIO4/GPIO11 shared with other profiles by design.
  // MFRC522 RFID remains global and NOT overlapped.

  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_DESERT;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;

  expectedCount_ = ProfileConfig::expectedSensorCount(ProfileConfig::Profile::Desert);

  // 0x0500 - BME280 air temperature.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_BME280, base + 0, period);
    c.pinSda = ProfileConfig::PROFILE4_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE4_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE4_BME280_I2C_ADDR;
    c.registerAddr = 0;  // temperature
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0501 - BME280 humidity.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_BME280, base + 1, period);
    c.pinSda = ProfileConfig::PROFILE4_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE4_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE4_BME280_I2C_ADDR;
    c.registerAddr = 1;  // humidity
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0502 - BME280 pressure.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_BME280, base + 2, period);
    c.pinSda = ProfileConfig::PROFILE4_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE4_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE4_BME280_I2C_ADDR;
    c.registerAddr = 2;  // pressure
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0503 - Sand surface temperature ADC.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,
                                base + 3, period);
    c.pinSda = ProfileConfig::PROFILE4_SAND_TEMP_ADC_PIN;  // GPIO4
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0504 - Soil moisture ADC via GPIO3 mux (Y1 = ADC branch).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_ADC,
                                base + 4, period);
    c.pinSda = ProfileConfig::PROFILE4_SOIL_MOISTURE_ADC_PIN;  // GPIO3
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0505 - VEML6075 solar/UV.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,
                                base + 5, period);
    c.pinSda = ProfileConfig::PROFILE4_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE4_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE4_VEML6075_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0506 - Dust & visibility (UART placeholder).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_DUST_VISIBILITY,
                                base + 6, period);
    c.dataWidth = 4;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0507 - Wind pulse counter (anemometer).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_PULSE_COUNTER,
                                base + 7, period);
    c.pinSda = ProfileConfig::PROFILE4_WIND_PULSE_PIN;  // GPIO11
    c.registerAddr = 1;  // wind
    c.dataWidth = 1;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Pulse);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  Serial.printf("PROFILE: desert roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}

bool ProfileSensors::registerMineTunnel(SensorDriverRegistry& drivers,
                                        SensorRegistry& registry) {
  // Profile 5 roster (10 sensors):
  //   0x0600 ch4              gas ADC       GPIO4
  //   0x0601 co               gas ADC       GPIO11
  //   0x0602 h2s              gas ADC       via I2C (placeholder)
  //   0x0603 co2              SCD4x I2C     I2C 0x62
  //   0x0604 o2               O2 UART       UART1 (placeholder)
  //   0x0605 air_temp         BME280        I2C 0x76
  //   0x0606 humidity         BME280        I2C 0x76 (shared)
  //   0x0607 pressure         BME280        I2C 0x76 (shared)
  //   0x0608 seismic_tilt     seismic SPI   GPIO3 via mux Y2
  //   0x0609 pm25             PMS I2C       I2C (placeholder)
  //
  // Pin overlap: GPIO3/GPIO4/GPIO11 shared with other profiles by design.
  // MFRC522 RFID remains global and NOT overlapped.

  const uint16_t base = ProfileConfig::SENSOR_ID_BASE_MINE_TUNNEL;
  const uint32_t period = SensorNodeConfig::SENSOR_SAMPLE_PERIOD_MS;

  expectedCount_ =
      ProfileConfig::expectedSensorCount(ProfileConfig::Profile::MineTunnel);

  // 0x0600 - CH4 gas sensor ADC.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GAS_ADC,
                                base + 0, period);
    c.pinSda = ProfileConfig::PROFILE5_CH4_ADC_PIN;  // GPIO4
    c.registerAddr = 0;  // CH4
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0601 - CO gas sensor ADC.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GAS_ADC,
                                base + 1, period);
    c.pinSda = ProfileConfig::PROFILE5_CO_ADC_PIN;  // GPIO11
    c.registerAddr = 1;  // CO
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::Adc);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0602 - H2S gas sensor (I2C placeholder).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,
                                base + 2, period);
    c.pinSda = ProfileConfig::PROFILE5_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE5_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE5_H2S_I2C_ADDR;
    c.dataWidth = 2;
    c.registerAddr = 2;  // H2S
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0603 - CO2 (SCD4x).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,
                                base + 3, period);
    c.pinSda = ProfileConfig::PROFILE5_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE5_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE5_CO2_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0604 - O2 sensor (UART placeholder).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_UART,
                                base + 4, period);
    c.dataWidth = 4;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::UART);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0605 - BME280 air temperature.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_BME280,
                                base + 5, period);
    c.pinSda = ProfileConfig::PROFILE5_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE5_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE5_BME280_I2C_ADDR;
    c.registerAddr = 0;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0606 - BME280 humidity.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_BME280,
                                base + 6, period);
    c.pinSda = ProfileConfig::PROFILE5_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE5_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE5_BME280_I2C_ADDR;
    c.registerAddr = 1;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0607 - BME280 pressure.
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_BME280,
                                base + 7, period);
    c.pinSda = ProfileConfig::PROFILE5_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE5_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE5_BME280_I2C_ADDR;
    c.registerAddr = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0608 - Seismic / tilt sensor via GPIO3 mux (Y2 = ADXL355 CS branch).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_SEISMIC_SPI,
                                base + 8, period);
    c.pinSda = ProfileConfig::PROFILE5_SEISMIC_CS_PIN;  // GPIO3
    c.pinScl = ProfileConfig::PROFILE5_SEISMIC_SCK_PIN;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::SPI);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  // 0x0609 - PM2.5/PM10 (PMS I2C placeholder).
  {
    DriverConfig c = makeConfig(SensorDriverRegistry::DRIVER_GENERIC_I2C,
                                base + 9, period);
    c.pinSda = ProfileConfig::PROFILE5_I2C_SDA_PIN;
    c.pinScl = ProfileConfig::PROFILE5_I2C_SCL_PIN;
    c.i2cAddr = ProfileConfig::PROFILE5_PMS_I2C_ADDR;
    c.dataWidth = 2;
    c.interfaceType = static_cast<uint8_t>(ProfileConfig::InterfaceKind::I2C);
    if (addDriver(drivers, registry, c)) ++registeredCount_;
  }

  Serial.printf("PROFILE: mine_tunnel roster %u/%u sensors\n",
                static_cast<unsigned>(registeredCount_),
                static_cast<unsigned>(expectedCount_));
  return registeredCount_ == expectedCount_;
}
