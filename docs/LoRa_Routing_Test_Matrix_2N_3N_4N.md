# FieldRadio Rev B — LoRa Routing Validation Test Matrix

## Tujuan

Membuktikan sebelum flashing ke ESP32-S3-WROOM-1 bahwa mekanisme:

- destination / next-hop payload extension
- ETX + RSSI/SNR route selection
- route aging
- loop prevention
- selective forwarding
- duplicate/replay handling
- TTL / hop-count enforcement

berjalan sesuai desain pada topologi 2-node, 3-node, dan 4-node.

## Baseline firmware

Target: ESP32-S3-WROOM-1 + SX1262.

Nilai konfigurasi yang menjadi baseline:

| Parameter | Nilai |
|---|---:|
| `LORA_INITIAL_TTL` | 3 |
| Route cache aging | 120 s |
| Dedup TTL | 300 s |
| Replay window | 32 sequence |
| Replay time window | 300 s |
| Forward rate limit | 1000 ms/source |
| Voice forward rate limit | 100 ms/source |
| Forward queue depth | 6 |
| Route cache size | 16 |
| Neighbor cache size | 16 |

> Untuk pengujian RF, gunakan dummy load/attenuator atau jarak aman yang sesuai regulasi dan kemampuan hardware. Jangan melakukan TX pada frekuensi yang tidak diizinkan secara lokal.

---

# 1. Definisi node

Gunakan ID tetap selama seluruh test.

| Node | Peran | ID contoh |
|---|---|---:|
| A | Source | `0x000000A1` |
| B | Relay kandidat 1 | `0x000000B1` |
| C | Relay kandidat 2 / destination | `0x000000C1` |
| D | Relay / destination | `0x000000D1` |

Untuk test berikut, semua node memakai key LoRa yang sama dan konfigurasi radio yang identik kecuali parameter yang sengaja diuji.

---

# 2. Instrumentasi minimum

Catat untuk setiap paket:

- timestamp
- source ID
- destination
- previous hop
- selected next-hop
- hop count
- TTL
- RSSI
- SNR
- TX attempt
- TX success / ACK
- ETX
- apakah packet delivered / forwarded / dropped
- alasan drop bila tersedia

Acceptance sebaiknya dilakukan dari serial log + packet capture/logging yang tersedia di firmware.

---

# 3. Test matrix 2-node

## 2N-01 — Direct destination

**Topologi**

`A <-> C`

**Langkah**

1. Boot A dan C.
2. Pastikan C muncul sebagai fresh neighbor A.
3. Kirim 20 text packet dari A ke C.
4. Catat next-hop untuk setiap packet.

**Expected**

- destination = C.
- next-hop = C.
- packet diterima C.
- tidak ada relay.
- hop count tetap 0 pada paket yang dibuat A.
- ACK kembali ke A.

**Pass criteria**

- 20/20 packet diterima, atau failure rate sesuai target RF yang ditetapkan.
- Tidak ada forwarding loop.

---

## 2N-02 — ETX degradation

**Topologi**

`A <-> C`

**Langkah**

1. Kirim 50 packet dalam kondisi link baik.
2. Simulasikan loss/intermittent ACK.
3. Kirim 50 packet lagi.
4. Bandingkan ETX sebelum dan sesudah.

**Expected**

- `txAttempts` bertambah setiap percobaan.
- `txSuccess` hanya bertambah pada keberhasilan.
- ETX meningkat ketika packet loss meningkat.
- RSSI/SNR tetap ikut memengaruhi ranking.

**Pass criteria**

- ETX tidak turun ketika success ratio memburuk.
- ETX tidak overflow.
- Nilai tetap terbatas pada `ROUTE_ETX_MAX_Q8`.

---

# 4. Test matrix 3-node

## 3N-01 — One-relay delivery

**Topologi**

`A -> B -> C`

**Langkah**

1. Pastikan A tidak memiliki direct usable route ke C.
2. Pastikan B memiliki link ke A dan C.
3. Kirim 20 text packet A -> C.
4. Periksa route extension pada setiap hop.

**Expected**

Pada A:

- destination = C
- next-hop = B
- previous-hop = A
- hop count awal = 0

Pada B:

- packet ditujukan ke B.
- B menerima dan memvalidasi authentication.
- B memilih next-hop = C.
- previous-hop menjadi B.
- hop count bertambah 1.
- C menerima packet.

**Pass criteria**

- C menerima packet.
- B hanya forwarding sekali untuk sequence yang sama.
- Tidak ada B -> A -> B loop.

---

## 3N-02 — Selective forwarding

**Topologi**

`A -> B -> C`

**Langkah**

1. Kirim destination-specific packet A -> C.
2. Pastikan B adalah next-hop.
3. Tambahkan node X/RF neighbor lain bila tersedia.
4. Amati forwarding.

**Expected**

- Hanya node yang memenuhi `nextHop == local sourceId` yang menjadi relay.
- Neighbor lain tidak ikut meneruskan destination-specific packet.
- Packet tidak berubah menjadi flooding broadcast.

**Pass criteria**

- Jumlah forwarding relay = minimum yang diperlukan.
- Tidak ada duplicate transmission dari node yang bukan next-hop.

---

## 3N-03 — Previous-hop exclusion

**Topologi**

`A -> B -> C`

dengan B juga mendengar A.

**Langkah**

1. B menerima packet dari A.
2. Paksa/atur kondisi agar A menjadi kandidat neighbor terkuat.
3. B memilih next-hop.

**Expected**

- B tidak memilih A sebagai next-hop untuk forwarding packet yang baru diterima dari A.
- Candidate A dikeluarkan melalui `excludeNextHop`.

**Pass criteria**

- next-hop != previous-hop.
- Tidak ada immediate bounce `B -> A`.

---

## 3N-04 — Route aging

**Topologi**

`A -> B -> C`

**Langkah**

1. Bangun route A -> C melalui B.
2. Verifikasi route aktif.
3. Hentikan beacon/traffic yang menyegarkan route.
4. Tunggu >120 s.
5. Kirim packet A -> C.

**Expected**

- Route lama dianggap stale.
- Route tidak dipilih lagi setelah TTL cache lewat.
- Jika tidak ada route baru, packet tidak dipaksa memakai stale next-hop.

**Pass criteria**

- Sebelum aging: route dipilih.
- Setelah >120 s tanpa refresh: route tidak dipilih.
- Tidak ada forwarding menggunakan route stale.

---

# 5. Test matrix 4-node

## 4N-01 — Two-relay path

**Topologi**

`A -> B -> C -> D`

Destination = D.

**Langkah**

1. Pastikan A -> B, B -> C, dan C -> D usable.
2. Pastikan direct A -> D tidak usable.
3. Kirim 20 packet A -> D.
4. Catat route extension pada A, B, C, D.

**Expected**

- A memilih B.
- B memilih C.
- C memilih D.
- D menerima packet.
- hop count bertambah setiap forwarding.
- TTL berkurang setiap forwarding.

**Pass criteria**

- Packet sampai D.
- Tidak ada relay tambahan.
- Tidak ada loop.
- Hop count tidak melebihi batas.

---

## 4N-02 — Loop injection: A -> B -> C -> B

**Topologi**

`A -> B -> C`

B dan C saling mendengar.

**Langkah**

1. Kirim packet destination D/unknown melalui B.
2. Buat kondisi di mana C memiliki peluang memilih B sebagai candidate.
3. Periksa exclusion previous-hop dan route validation.

**Expected**

- C tidak memilih B bila B adalah previous-hop.
- `routeAllowsForward()` menolak kondisi destination/next-hop/previous-hop yang tidak valid.
- Packet tidak berputar C -> B -> C.

**Pass criteria**

- Tidak ada repeated sequence tanpa batas.
- Hop count selalu meningkat.
- Packet akhirnya delivered atau dropped, bukan looping.

---

## 4N-03 — TTL exhaustion

**Topologi**

`A -> B -> C -> D`

**Langkah**

1. Inject packet dengan TTL = 1 menuju D.
2. B menerima packet.
3. B mencoba forwarding.

**Expected**

- Packet tidak diteruskan ketika TTL tidak cukup.
- Tidak ada packet dengan TTL 0 yang ditransmisikan.

**Pass criteria**

- Forwarding ditolak.
- Tidak ada TX dari B untuk packet tersebut.

---

## 4N-04 — Better ETX beats weaker link

**Topologi**

```
        B
       / \
A ----    ---- D
       \ /
        C
```

A memiliki dua kandidat: B dan C.

**Langkah**

1. Buat B memiliki RSSI/SNR sedikit lebih baik tetapi packet loss tinggi.
2. Buat C memiliki RSSI/SNR sedikit lebih rendah tetapi delivery ratio jauh lebih baik.
3. Bangun statistik TX.
4. Kirim 50–100 packet A -> D.
5. Catat selected next-hop.

**Expected**

- ETX penalti link B meningkat.
- C dapat mengungguli B walaupun RSSI/SNR C sedikit lebih buruk.
- Route selection tidak hanya berdasarkan RSSI.

**Pass criteria**

- Setelah statistik stabil, kandidat dengan composite score lebih baik dipilih secara konsisten.
- Perubahan ranking dapat diamati setelah ETX berubah.

---

## 4N-05 — Better RSSI/SNR with equal ETX

**Topologi**

A memiliki kandidat B dan C yang mempunyai ETX setara.

**Langkah**

1. Stabilkan TX success ratio B dan C agar ETX mendekati sama.
2. Buat RSSI/SNR B lebih baik.
3. Kirim packet A -> D.

**Expected**

- B mendapat quality score lebih tinggi.
- B dipilih sebagai next-hop.

**Pass criteria**

- next-hop = B.
- Perubahan RSSI/SNR yang cukup besar menyebabkan perubahan ranking.

---

# 6. Security / robustness tests

## SEC-01 — Duplicate packet

Kirim packet yang sama dua kali.

**Expected:** packet kedua tidak diteruskan/diproses sebagai packet baru.

**Pass:** duplicate tidak menghasilkan duplicate application delivery.

---

## SEC-02 — Same sequence, modified routing extension

Gunakan sequence yang sama tetapi ubah destination/next-hop/previous-hop.

**Expected:** authentication harus gagal bila ciphertext/tag tidak valid; packet tidak boleh dipercaya hanya karena sequence cocok.

**Pass:** packet invalid tidak diteruskan.

---

## SEC-03 — Wrong next-hop

Destination = D, next-hop = X, tetapi packet diterima B.

**Expected:** B tidak memproses sebagai addressed-to-us dan tidak melakukan selective forwarding bila B bukan next-hop.

**Pass:** no forward from B.

---

## SEC-04 — Self destination

Destination = local node.

**Expected:** node menerima packet untuk application layer dan tidak meneruskannya.

**Pass:** no forwarding.

---

## SEC-05 — Self previous-hop

previous-hop = local node.

**Expected:** `routeAllowsForward()` menolak forwarding.

**Pass:** packet dropped.

---

## SEC-06 — Excessive hop count

Inject authenticated packet dengan hop count >= `LORA_INITIAL_TTL`.

**Expected:** forwarding ditolak.

**Pass:** no TX.

---

# 7. Route-aging timing test

Gunakan stopwatch/serial timestamp.

| Waktu sejak refresh | Expected route state |
|---:|---|
| 0 s | Fresh |
| 30 s | Fresh |
| 60 s | Fresh |
| 119 s | Fresh |
| 120 s | Boundary — jangan bergantung pada exact millisecond |
| >120 s | Stale / unusable |

Untuk automated acceptance, gunakan margin aman:

- PASS fresh: <110 s
- PASS stale: >130 s

Margin ini menghindari false result akibat scheduling FreeRTOS dan timestamp sampling.

---

# 8. ETX acceptance test

Gunakan minimal 50 TX attempts per kondisi.

| Kondisi | Attempts | Success | Expected ETX trend |
|---|---:|---:|---|
| Excellent | 50 | 48–50 | sekitar 1.00–1.04 |
| Good | 50 | 40–47 | sekitar 1.06–1.25 |
| Medium | 50 | 25–39 | sekitar 1.28–2.00 |
| Poor | 50 | 10–24 | >2.00 |
| Dead | 50 | 0 | worst-case / rejected |

Nilai aktual boleh berbeda karena integer Q8 calculation, tetapi ranking kualitas harus konsisten.

---

# 9. Selective-forwarding acceptance

Untuk satu packet destination-specific, hitung TX:

**3-node A-B-C**

Expected:

- A: 1 TX initial
- B: maksimal 1 forwarding TX untuk sequence tersebut
- C: 0 forwarding TX

**4-node A-B-C-D**

Expected:

- A: 1 TX initial
- B: maksimal 1 forwarding TX
- C: maksimal 1 forwarding TX
- D: 0 forwarding TX

Retry radio/ACK tidak dihitung sebagai forwarding baru bila masih merupakan retry dari hop yang sama.

---

# 10. Final go/no-go matrix

| Test | 2N | 3N | 4N | Mandatory |
|---|:---:|:---:|:---:|:---:|
| Direct destination | PASS | PASS | PASS | YES |
| ETX calculation | PASS | PASS | PASS | YES |
| RSSI/SNR ranking | PASS | PASS | PASS | YES |
| ETX vs RSSI/SNR composite selection | PASS | PASS | PASS | YES |
| Selective forwarding | N/A | PASS | PASS | YES |
| Previous-hop exclusion | N/A | PASS | PASS | YES |
| Route aging | N/A | PASS | PASS | YES |
| Loop prevention | N/A | PASS | PASS | YES |
| TTL exhaustion | N/A | PASS | PASS | YES |
| Duplicate suppression | PASS | PASS | PASS | YES |
| Invalid route extension | PASS | PASS | PASS | YES |
| Wrong next-hop | N/A | PASS | PASS | YES |
| Self-destination no-forward | PASS | PASS | PASS | YES |

## GO criteria

Firmware boleh masuk tahap flashing/hardware validation hanya jika:

1. Semua test **Mandatory = PASS**.
2. Tidak ada forwarding loop.
3. Tidak ada packet dengan TTL 0 yang ditransmisikan.
4. Route stale tidak dipakai.
5. Previous-hop tidak dipilih kembali sebagai next-hop.
6. Node yang bukan next-hop tidak melakukan selective forwarding.
7. ETX membedakan link loss yang nyata.
8. RSSI/SNR tetap berpengaruh ketika ETX setara.
9. Destination-specific packet tidak berubah menjadi flooding.
10. Duplicate packet tidak menyebabkan duplicate application delivery.

## Evidence yang disimpan

Untuk setiap test simpan:

- `serial_A.log`
- `serial_B.log`
- `serial_C.log`
- `serial_D.log` bila 4-node
- konfigurasi radio
- firmware commit/hash
- timestamp test
- tabel packet ID / sequence
- RSSI/SNR
- ETX sebelum/sesudah
- next-hop yang dipilih
- hasil PASS/FAIL

## Catatan

Test ini memvalidasi perilaku routing firmware. Ia bukan pengganti RF certification, antenna validation, conducted power measurement, regulatory testing, atau production EMC testing.
