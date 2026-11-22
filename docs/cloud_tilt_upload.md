# Cloud tilt upload

Cloud firmware now persists the existing filtered Roll/Pitch estimate at sampling
time and uploads degrees using `compact-v3-tilt`. Roll is -180..180 degrees;
Pitch is -90..90 degrees. No yaw is inferred from this accelerometer.

The existing startup calibration assumes the board is stationary and level
(X/Y horizontal, Z along gravity) for the first 2 seconds. These are approximate
gravity-based tilt estimates; motion/impact can distort them. This change does
not add a gyroscope, six-position calibration, or change event detection.
Uncalibrated/invalid samples upload null angles and false validity.

Flash records remain 90 bytes. For cloud sensor snapshots, reserved[0]=1 and
the old accel slots store round((roll+180)*100), round((pitch+90)*100), valid.
Legacy records keep reserved[0]=0. Existing queued records are uploaded as raw
ADC with unchanged normalized payload hashes, so retries remain idempotent.
The non-cloud firmware continues recording ADC. Do not downgrade to old firmware
or a PC archive uploader while tilt records are pending: those versions do not
understand the new marker.

Wire records retain the first 14 positions, with positions 2/3/4 meaning
roll_deg/pitch_deg/tilt_valid when the appended position 14 encoding is 1.
Encoding 0 keeps legacy ADC semantics. Mixed batches support firmware upgrades
with old records still in Flash. Sensor API payloads expose roll_deg, pitch_deg,
tilt_valid instead of accel_x/y/z. GPS-only and shot-only rows keep their existing
semantics. Viewer columns show Roll/Pitch; the validity / historical ADC column has been removed.

Deploy the receiver and viewer first, then flash the esp32-s3-cloud firmware.
The old receiver rejects compact-v3-tilt; the device retains rejected records
in Flash for retry. No historical database rewrite is needed.

The newer `compact-v4-time` format preserves these angle fields and adds inferred capture time provenance. See [capture-time inference](inferred_capture_time.md).
