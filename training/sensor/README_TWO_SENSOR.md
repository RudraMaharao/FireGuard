# Revision-2 sensor data

The production firmware uses two_sensor_model.h, not the archived three-sensor Model A. The 56-position serial schema retains positions for capture stability but renames removed sensor quantities unused_mq7, unused_r7, unused_sc7, unused_co, unused_mco, unused_codose and unused_crs. These quantities are nan. The internal fusion CO confidence slot is disabled. signal2/signal135 are nominal voltage-odds ratios, not calibrated Rs/R0 or ppm.

Use this release's serial_capture.py to record labelled data. Old train_sensor_model.py and calibrate_fusion.py CLI entry points deliberately refuse to run for this hardware. A future trained classifier needs a new two-channel feature schema, labels, grouped train/validation/test split and measured performance before replacement. Camera training scripts remain available; the included fire/smoke CNN weights are retained unchanged by this release.
