import glob
import math
import os
from pathlib import Path
import random
import cv2
import numpy as np
import tensorflow as tf
from tensorflow.keras import layers, models

# 📌 OpenCV 멀티스레드 락으로 인한 Epoch 멈춤 현상 방지
cv2.setNumThreads(0)

# ── 1. 경로 및 하이퍼파라미터 설정 ───────────────────────────────────────────
DATASET_DIR = "/content/busdet"

TRAIN_IMG = os.path.join(DATASET_DIR, "train/images")
TRAIN_LBL = os.path.join(DATASET_DIR, "train/labels")
VAL_IMG = os.path.join(DATASET_DIR, "val/images")
VAL_LBL = os.path.join(DATASET_DIR, "val/labels")

SAVE_DIR = "/content/drive/MyDrive/bus_number_model"
os.makedirs(SAVE_DIR, exist_ok=True)

IMG_SIZE = 224
GRID_SIZE = 14  # 224 / 16 = 14x14 Grid
BATCH_SIZE = 32
EPOCHS = 100
LR_INIT = 2e-4
EXTS = ("*.jpeg", "*.jpg", "*.png", "*.bmp")


# ── 2. Center Target Encoder (5채널: dx, dy, dw, dh, conf) ───────────────────
def encode_bus_target_multi(boxes):
    target = np.zeros((GRID_SIZE, GRID_SIZE, 5), dtype=np.float32)

    for box in boxes:
        cx, cy, bw, bh = box
        gx = int(cx * GRID_SIZE)
        gy = int(cy * GRID_SIZE)
        gx = min(max(0, gx), GRID_SIZE - 1)
        gy = min(max(0, gy), GRID_SIZE - 1)

        dx = cx * GRID_SIZE - gx
        dy = cy * GRID_SIZE - gy

        target[gy, gx] = [dx, dy, bw, bh, 1.0]

    return target


# ── 3. 데이터 로드 및 증강 ──────────────────────────────────────────────────
def load_sample_bus(img_path, lbl_path, augment=False):
    img = cv2.imread(str(img_path), cv2.IMREAD_GRAYSCALE)
    if img is None:
        img = np.zeros((IMG_SIZE, IMG_SIZE), dtype=np.uint8)
    else:
        img = cv2.resize(img, (IMG_SIZE, IMG_SIZE))

    boxes = []
    if os.path.exists(lbl_path):
        with open(lbl_path, "r") as f:
            for line in f:
                p = line.strip().split()
                if len(p) == 5:
                    boxes.append(
                        [float(p[1]), float(p[2]), float(p[3]), float(p[4])]
                    )

    if augment:
        if random.random() < 0.5:
            img = cv2.flip(img, 1)
            boxes = [[1.0 - b[0], b[1], b[2], b[3]] for b in boxes]

        alpha = random.uniform(0.7, 1.3)
        beta = random.uniform(-30, 30)
        img = np.clip(img.astype(np.float32) * alpha + beta, 0, 255).astype(
            np.uint8
        )

        if random.random() < 0.4:
            noise = np.random.normal(0, 10, img.shape).astype(np.float32)
            img = np.clip(img.astype(np.float32) + noise, 0, 255).astype(
                np.uint8
            )

        if random.random() < 0.3:
            img = cv2.GaussianBlur(img, (3, 3), 0)

    img_f = img.astype(np.float32)[..., np.newaxis] / 255.0
    target_grid = encode_bus_target_multi(boxes)
    return img_f, target_grid


def parse_fn_bus(p_bin, l_bin, aug_bin):
    img_f, target = load_sample_bus(
        p_bin.numpy().decode("utf-8"),
        l_bin.numpy().decode("utf-8"),
        bool(aug_bin.numpy()),
    )
    return img_f, target


def make_dataset(img_dir, lbl_dir, augment, batch, shuffle):
    paths = sorted(sum([glob.glob(f"{img_dir}/{e}") for e in EXTS], []))
    labels = [f"{lbl_dir}/{Path(p).stem}.txt" for p in paths]

    ds = tf.data.Dataset.from_tensor_slices((paths, labels))
    if shuffle:
        ds = ds.shuffle(buffer_size=len(paths), reshuffle_each_iteration=True)

    def _map_fn(p, l):
        img_f, target = tf.py_function(
            parse_fn_bus, [p, l, augment], [tf.float32, tf.float32]
        )
        img_f.set_shape((IMG_SIZE, IMG_SIZE, 1))
        target.set_shape((GRID_SIZE, GRID_SIZE, 5))
        return img_f, target

    return (
        ds.map(_map_fn, num_parallel_calls=4)
        .batch(batch)
        .repeat()
        .prefetch(tf.data.AUTOTUNE),
        len(paths),
    )


train_ds, N_TRAIN = make_dataset(
    TRAIN_IMG, TRAIN_LBL, augment=True, batch=BATCH_SIZE, shuffle=True
)
val_ds, N_VAL = make_dataset(
    VAL_IMG, VAL_LBL, augment=False, batch=BATCH_SIZE, shuffle=False
)

STEPS_PER_EPOCH = math.ceil(N_TRAIN / BATCH_SIZE)
VALIDATION_STEPS = math.ceil(N_VAL / BATCH_SIZE)


# ── 4. ESP32-S3 경량 모델 구조 (Lambda 제거 / 흑백 전용) ─────────────────────
def build_bus_number_model():
    inp = layers.Input(shape=(IMG_SIZE, IMG_SIZE, 1), name="image_input")
    x3 = layers.Concatenate(axis=-1, name="rgb_concat")([inp, inp, inp])

    base_mbv2 = tf.keras.applications.MobileNetV2(
        alpha=0.35,
        include_top=False,
        input_shape=(IMG_SIZE, IMG_SIZE, 3),
        weights="imagenet",
    )
    base_mbv2.trainable = True
    feature_map = base_mbv2.get_layer("block_12_add").output
    backbone = models.Model(
        inputs=base_mbv2.input, outputs=feature_map, name="mbv2_14x14"
    )

    x = backbone(x3)
    x = layers.Conv2D(
        64, 3, padding="same", activation="relu", name="head_conv"
    )(x)
    out = layers.Conv2D(5, 1, activation="sigmoid", name="box_head")(x)

    return models.Model(inputs=inp, outputs=out, name="Bus_Number_CenterNet")


model = build_bus_number_model()


# ── 5. Loss & Metrics (IoU 평가 지표 포함) ───────────────────────────────────
def bus_number_loss(y_true, y_pred):
    obj_mask = y_true[..., 4:5]
    noobj_mask = 1.0 - obj_mask

    num_obj = tf.reduce_sum(obj_mask) + 1e-7
    num_noobj = tf.reduce_sum(noobj_mask) + 1e-7

    offset_loss = (
        tf.reduce_sum(obj_mask * tf.square(y_true[..., 0:2] - y_pred[..., 0:2]))
        / num_obj
    )
    size_loss = (
        tf.reduce_sum(obj_mask * tf.abs(y_true[..., 2:4] - y_pred[..., 2:4]))
        / num_obj
    )

    p = tf.clip_by_value(y_pred[..., 4:5], 1e-7, 1.0 - 1e-7)
    obj_focal = -tf.pow(1.0 - p, 2.0) * tf.math.log(p)
    noobj_focal = -tf.pow(p, 3.0) * tf.math.log(1.0 - p)

    obj_loss = tf.reduce_sum(obj_mask * obj_focal) / num_obj
    noobj_loss = tf.reduce_sum(noobj_mask * noobj_focal) / num_noobj

    return 2.0 * offset_loss + 2.0 * size_loss + 1.0 * obj_loss + 8.0 * noobj_loss


# 📌 [핵심 지표] Bounding Box IoU (Intersection over Union, 1.0에 가까울수록 정밀)
def box_iou_metric(y_true, y_pred):
    obj_mask = y_true[..., 4:5]
    num_obj = tf.reduce_sum(obj_mask) + 1e-7

    gx = tf.cast(tf.range(GRID_SIZE), tf.float32)
    gy = tf.cast(tf.range(GRID_SIZE), tf.float32)
    gx_grid, gy_grid = tf.meshgrid(gx, gy)
    gx_grid = tf.expand_dims(gx_grid, axis=-1)
    gy_grid = tf.expand_dims(gy_grid, axis=-1)

    # Ground Truth Box Coordinates
    cx_t = (gx_grid + y_true[..., 0:1]) / float(GRID_SIZE)
    cy_t = (gy_grid + y_true[..., 1:2]) / float(GRID_SIZE)
    w_t, h_t = y_true[..., 2:3], y_true[..., 3:4]

    t_x1, t_y1 = cx_t - w_t / 2.0, cy_t - h_t / 2.0
    t_x2, t_y2 = cx_t + w_t / 2.0, cy_t + h_t / 2.0

    # Prediction Box Coordinates
    cx_p = (gx_grid + y_pred[..., 0:1]) / float(GRID_SIZE)
    cy_p = (gy_grid + y_pred[..., 1:2]) / float(GRID_SIZE)
    w_p, h_p = y_pred[..., 2:3], y_pred[..., 3:4]

    p_x1, p_y1 = cx_p - w_p / 2.0, cy_p - h_p / 2.0
    p_x2, p_y2 = cx_p + w_p / 2.0, cy_p + h_p / 2.0

    # Intersection
    i_x1 = tf.maximum(t_x1, p_x1)
    i_y1 = tf.maximum(t_y1, p_y1)
    i_x2 = tf.minimum(t_x2, p_x2)
    i_y2 = tf.minimum(t_y2, p_y2)

    i_w = tf.maximum(0.0, i_x2 - i_x1)
    i_h = tf.maximum(0.0, i_y2 - i_y1)
    intersection = i_w * i_h

    # Union
    union = (w_t * h_t) + (w_p * h_p) - intersection + 1e-7

    iou = intersection / union
    return tf.reduce_sum(obj_mask * iou) / num_obj


def pos_conf_metric(y_true, y_pred):
    obj_mask = y_true[..., 4:5]
    num_obj = tf.reduce_sum(obj_mask) + 1e-7
    return tf.reduce_sum(obj_mask * y_pred[..., 4:5]) / num_obj


def neg_conf_metric(y_true, y_pred):
    noobj_mask = 1.0 - y_true[..., 4:5]
    num_noobj = tf.reduce_sum(noobj_mask) + 1e-7
    return tf.reduce_sum(noobj_mask * y_pred[..., 4:5]) / num_noobj


model.compile(
    optimizer=tf.keras.optimizers.Adam(LR_INIT),
    loss=bus_number_loss,
    metrics=[box_iou_metric, pos_conf_metric, neg_conf_metric],
)

# ── 6. 학습 ──────────────────────────────────────────────────────────────────
callbacks = [
    tf.keras.callbacks.ModelCheckpoint(
        f"{SAVE_DIR}/best.keras",
        monitor="val_loss",
        save_best_only=True,
        verbose=1,
    ),
    tf.keras.callbacks.ReduceLROnPlateau(
        monitor="val_loss", factor=0.5, patience=5, min_lr=1e-6, verbose=1
    ),
    tf.keras.callbacks.EarlyStopping(
        monitor="val_loss", patience=12, restore_best_weights=True, verbose=1
    ),
]

print("\n🚀 버스 번호판 검출 테스트 모델 학습 시작...")
model.fit(
    train_ds,
    validation_data=val_ds,
    epochs=EPOCHS,
    steps_per_epoch=STEPS_PER_EPOCH,
    validation_steps=VALIDATION_STEPS,
    callbacks=callbacks,
    verbose=1,
)

# ── 7. Pure INT8 TFLite 변환 ─────────────────────────────────────────────────
print("\n=== Pure INT8 TFLite 변환 시작 ===")
converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]


def representative_dataset_gen():
    for imgs, _ in val_ds.unbatch().batch(1).take(200):
        yield [imgs]


converter.representative_dataset = representative_dataset_gen
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8

tflite_model = converter.convert()
tflite_path = f"{SAVE_DIR}/bus_number_pure_int8.tflite"

with open(tflite_path, "wb") as f:
    f.write(tflite_model)

print(f"\n🎉 변환 성공! Pure INT8 버스 모델 저장 완료: {tflite_path}")
print(f"📦 최종 파일 크기: {len(tflite_model)/1024:.1f} KB")