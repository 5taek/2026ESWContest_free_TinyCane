import math
import os, glob, random
from pathlib import Path
import cv2
import numpy as np
import tensorflow as tf
from tensorflow.keras import layers, models

# 📌 [핵심] OpenCV 멀티스레드 락 방지 (Epoch 1 멈춤 완벽 해결)
cv2.setNumThreads(0)

# ── 1. 경로 설정 (점자블록 데이터셋 + 하드 네거티브 데이터셋 분리) ───────────────
TRAIN_IMG = "/content/final_dataset/images/train"
TRAIN_LBL = "/content/final_dataset/labels/train"
VAL_IMG = "/content/final_dataset/images/val"
VAL_LBL = "/content/final_dataset/labels/val"

HARD_TRAIN_IMG = "/content/grayscale/train/images"
HARD_TRAIN_LBL = "/content/grayscale/train/labels"
HARD_VAL_IMG = "/content/grayscale/val/images"
HARD_VAL_LBL = "/content/grayscale/val/labels"

SAVE_DIR = "/content/drive/MyDrive/brailblock/model_final3"
os.makedirs(SAVE_DIR, exist_ok=True)

IMG_SIZE = 224
GRID_SIZE = 14  # 224 / 16 = 14x14 Grid
BATCH_SIZE = 32
EPOCHS = 100
LR_INIT = 2e-4
EXTS = ("*.jpeg", "*.jpg", "*.png", "*.bmp")


# ── 2. Target Encoder (오탐 방지 단일 셀 타깃) ─────────────────────────────────
def encode_yolo_target(box, has_obj):
    target = np.zeros((GRID_SIZE, GRID_SIZE, 5), dtype=np.float32)
    if has_obj > 0.5:
        cx, cy, w, h = box
        gx = int(cx * GRID_SIZE)
        gy = int(cy * GRID_SIZE)
        gx = min(max(0, gx), GRID_SIZE - 1)
        gy = min(max(0, gy), GRID_SIZE - 1)

        dx = cx * GRID_SIZE - gx
        dy = cy * GRID_SIZE - gy
        target[gy, gx] = [dx, dy, w, h, 1.0]
    return target


# ── 3. 데이터 로더 & 파이프라인 (안정화 적용) ──────────────────────────────────
def load_sample(img_path, lbl_path, augment=False):
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

    if boxes:
        boxes.sort(key=lambda b: b[2] * b[3], reverse=True)
        box = boxes[0]
        has_obj = 1.0
    else:
        box = [0.0, 0.0, 0.0, 0.0]
        has_obj = 0.0

    if augment:
        if random.random() < 0.5:
            img = cv2.flip(img, 1)
            if has_obj:
                box = [1.0 - box[0], box[1], box[2], box[3]]
        alpha = random.uniform(0.8, 1.2)
        beta = random.uniform(-20, 20)
        img = np.clip(img.astype(np.float32) * alpha + beta, 0, 255).astype(
            np.uint8
        )

    img_f = img.astype(np.float32)[..., np.newaxis] / 255.0
    target_grid = encode_yolo_target(box, has_obj)
    return img_f, target_grid


def parse_fn(p_bin, l_bin, aug_bin):
    img_f, target = load_sample(
        p_bin.numpy().decode("utf-8"),
        l_bin.numpy().decode("utf-8"),
        bool(aug_bin.numpy()),
    )
    return img_f, target


def make_combined_dataset(img_dirs, lbl_dirs, augment, batch, shuffle):
    all_paths = []
    all_labels = []

    for img_dir, lbl_dir in zip(img_dirs, lbl_dirs):
        if not os.path.exists(img_dir):
            continue
        paths = sorted(sum([glob.glob(f"{img_dir}/{e}") for e in EXTS], []))
        labels = [f"{lbl_dir}/{Path(p).stem}.txt" for p in paths]
        all_paths.extend(paths)
        all_labels.extend(labels)

    ds = tf.data.Dataset.from_tensor_slices((all_paths, all_labels))
    if shuffle:
        ds = ds.shuffle(
            buffer_size=len(all_paths), reshuffle_each_iteration=True
        )

    def _map_fn(p, l):
        img_f, target = tf.py_function(
            parse_fn, [p, l, augment], [tf.float32, tf.float32]
        )
        img_f.set_shape((IMG_SIZE, IMG_SIZE, 1))
        target.set_shape((GRID_SIZE, GRID_SIZE, 5))
        return img_f, target

    total_count = len(all_paths)
    return (
        ds.map(_map_fn, num_parallel_calls=4)
        .batch(batch)
        .repeat()
        .prefetch(tf.data.AUTOTUNE),
        total_count,
    )


train_ds, N_TRAIN = make_combined_dataset(
    [TRAIN_IMG, HARD_TRAIN_IMG],
    [TRAIN_LBL, HARD_TRAIN_LBL],
    augment=True,
    batch=BATCH_SIZE,
    shuffle=True,
)
val_ds, N_VAL = make_combined_dataset(
    [VAL_IMG, HARD_VAL_IMG],
    [VAL_LBL, HARD_VAL_LBL],
    augment=False,
    batch=BATCH_SIZE,
    shuffle=False,
)

STEPS_PER_EPOCH = math.ceil(N_TRAIN / BATCH_SIZE)
VALIDATION_STEPS = math.ceil(N_VAL / BATCH_SIZE)

print(f"📊 결합된 총 학습 데이터: {N_TRAIN}장 | 총 검증 데이터: {N_VAL}장")


# ── 4. ESP32-S3 Clean & Ultra-Light 모델 ─────────────────────────────────────
def build_brailledet_v1_clean():
    inp = layers.Input(shape=(IMG_SIZE, IMG_SIZE, 1), name="image_input")
    x3 = layers.Concatenate(axis=-1, name="rgb_concat")([inp, inp, inp])

    # MobileNetV2(weights="imagenet")는 -1~1 입력을 기대한다.
    # 0~1로 로딩된 이미지를 여기서 -1~1로 맞춰줘야 pretrained weight가 제대로 작동한다.
    x3 = x3 * 2.0 - 1.0

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
    x = layers.Conv2D(64, 3, padding="same", activation="relu", name="head_conv")(
        x
    )
    out = layers.Conv2D(5, 1, activation="sigmoid", name="yolo_head")(x)

    return models.Model(inputs=inp, outputs=out, name="BrailleDet_v1_Clean")


model = build_brailledet_v1_clean()


# ── 5. Loss & Metrics ────────────────────────────────────────────────────────
def yolo_ciou_focal_loss(y_true, y_pred):
    obj_mask = y_true[..., 4:5]
    noobj_mask = 1.0 - obj_mask

    grid_y, grid_x = tf.meshgrid(
        tf.range(GRID_SIZE, dtype=tf.float32),
        tf.range(GRID_SIZE, dtype=tf.float32),
    )
    grid_x = tf.reshape(grid_x, (1, GRID_SIZE, GRID_SIZE, 1))
    grid_y = tf.reshape(grid_y, (1, GRID_SIZE, GRID_SIZE, 1))

    pred_cx = (y_pred[..., 0:1] + grid_x) / GRID_SIZE
    pred_cy = (y_pred[..., 1:2] + grid_y) / GRID_SIZE
    pred_w = tf.maximum(1e-6, y_pred[..., 2:3])
    pred_h = tf.maximum(1e-6, y_pred[..., 3:4])

    true_cx = (y_true[..., 0:1] + grid_x) / GRID_SIZE
    true_cy = (y_true[..., 1:2] + grid_y) / GRID_SIZE
    true_w = tf.maximum(1e-6, y_true[..., 2:3])
    true_h = tf.maximum(1e-6, y_true[..., 3:4])

    p_x1, p_y1 = pred_cx - pred_w / 2.0, pred_cy - pred_h / 2.0
    p_x2, p_y2 = pred_cx + pred_w / 2.0, pred_cy + pred_h / 2.0
    t_x1, t_y1 = true_cx - true_w / 2.0, true_cy - true_h / 2.0
    t_x2, t_y2 = true_cx + true_w / 2.0, true_cy + true_h / 2.0

    inter_w = tf.maximum(0.0, tf.minimum(p_x2, t_x2) - tf.maximum(p_x1, t_x1))
    inter_h = tf.maximum(0.0, tf.minimum(p_y2, t_y2) - tf.maximum(p_y1, t_y1))
    inter_area = inter_w * inter_h
    union_area = (pred_w * pred_h) + (true_w * true_h) - inter_area + 1e-7
    iou = inter_area / union_area

    c_x1 = tf.minimum(p_x1, t_x1)
    c_y1 = tf.minimum(p_y1, t_y1)
    c_x2 = tf.maximum(p_x2, t_x2)
    c_y2 = tf.maximum(p_y2, t_y2)
    c2 = tf.square(c_x2 - c_x1) + tf.square(c_y2 - c_y1) + 1e-7

    rho2 = tf.square(pred_cx - true_cx) + tf.square(pred_cy - true_cy)
    v = (4.0 / (math.pi**2)) * tf.square(
        tf.atan(true_w / (true_h + 1e-7)) - tf.atan(pred_w / (pred_h + 1e-7))
    )
    alpha = v / (1.0 - iou + v + 1e-7)

    ciou = iou - (rho2 / c2 + alpha * v)
    num_obj = tf.reduce_sum(obj_mask) + 1e-7
    ciou_loss = tf.reduce_sum(obj_mask * (1.0 - ciou)) / num_obj

    p = tf.clip_by_value(y_pred[..., 4:5], 1e-7, 1.0 - 1e-7)
    obj_focal = -tf.pow(1.0 - p, 2.0) * tf.math.log(p)
    noobj_focal = -tf.pow(p, 2.0) * tf.math.log(1.0 - p)

    obj_loss = tf.reduce_sum(obj_mask * obj_focal) / num_obj
    noobj_loss = tf.reduce_sum(noobj_mask * noobj_focal) / (
        tf.reduce_sum(noobj_mask) + 1e-7
    )

    # ── Hard Negative 전용 최댓값 페널티 ────────────────────────────────
    # 객체가 아예 없는 이미지(HN)에서, 196칸 평균으로 희석되지 않도록
    # 그리드 전체에서 confidence가 가장 높은 셀 하나만 직접 처벌한다.
    has_obj_image = tf.reduce_max(obj_mask, axis=[1, 2, 3])          # (batch,) 1=객체있음
    is_hard_neg = 1.0 - has_obj_image                                 # (batch,) 1=HN
    max_conf_per_image = tf.reduce_max(y_pred[..., 4], axis=[1, 2])   # (batch,)
    max_conf_per_image = tf.clip_by_value(max_conf_per_image, 1e-7, 1.0 - 1e-7)
    hard_neg_focal = -tf.pow(max_conf_per_image, 2.0) * tf.math.log(1.0 - max_conf_per_image)
    hard_neg_loss = tf.reduce_sum(is_hard_neg * hard_neg_focal) / (
        tf.reduce_sum(is_hard_neg) + 1e-7
    )

    return (
        5.0 * ciou_loss
        + 2.0 * obj_loss
        + 3.0 * noobj_loss      # 1.0 → 3.0 (소폭 인상)
        + 3.0 * hard_neg_loss   # 신규 항: HN 오검출 직접 억제
    )


def center_point_error(y_true, y_pred):
    """객체가 있는 셀에서 예측 중심점과 정답 중심점 사이 거리 (0~1 정규화 좌표 기준)."""
    obj_mask = y_true[..., 4:5]

    grid_y, grid_x = tf.meshgrid(
        tf.range(GRID_SIZE, dtype=tf.float32),
        tf.range(GRID_SIZE, dtype=tf.float32),
    )
    grid_x = tf.reshape(grid_x, (1, GRID_SIZE, GRID_SIZE, 1))
    grid_y = tf.reshape(grid_y, (1, GRID_SIZE, GRID_SIZE, 1))

    pred_cx = (y_pred[..., 0:1] + grid_x) / GRID_SIZE
    pred_cy = (y_pred[..., 1:2] + grid_y) / GRID_SIZE
    true_cx = (y_true[..., 0:1] + grid_x) / GRID_SIZE
    true_cy = (y_true[..., 1:2] + grid_y) / GRID_SIZE

    dist = tf.sqrt(tf.square(pred_cx - true_cx) + tf.square(pred_cy - true_cy) + 1e-9)
    return tf.reduce_sum(obj_mask * dist) / (tf.reduce_sum(obj_mask) + 1e-7)


def presence_accuracy(y_true, y_pred, thresh=0.5):
    true_present = tf.reduce_max(y_true[..., 4:5], axis=[1, 2])
    pred_present = tf.cast(
        tf.reduce_max(y_pred[..., 4:5], axis=[1, 2]) > thresh, tf.float32
    )
    correct = tf.cast(tf.equal(pred_present, true_present), tf.float32)
    return tf.reduce_mean(correct)


def presence_precision(y_true, y_pred, thresh=0.5):
    true_present = tf.reduce_max(y_true[..., 4:5], axis=[1, 2])
    pred_present = tf.cast(
        tf.reduce_max(y_pred[..., 4:5], axis=[1, 2]) > thresh, tf.float32
    )
    tp = tf.reduce_sum(pred_present * true_present)
    fp = tf.reduce_sum(pred_present * (1.0 - true_present))
    return tp / (tp + fp + 1e-7)


def presence_recall(y_true, y_pred, thresh=0.5):
    true_present = tf.reduce_max(y_true[..., 4:5], axis=[1, 2])
    pred_present = tf.cast(
        tf.reduce_max(y_pred[..., 4:5], axis=[1, 2]) > thresh, tf.float32
    )
    tp = tf.reduce_sum(pred_present * true_present)
    fn = tf.reduce_sum((1.0 - pred_present) * true_present)
    return tp / (tp + fn + 1e-7)


model.compile(
    optimizer=tf.keras.optimizers.Adam(LR_INIT),
    loss=yolo_ciou_focal_loss,
    metrics=[center_point_error, presence_accuracy, presence_precision, presence_recall],
)

# ── 6. 콜백 및 Early Stopping ────────────────────────────────────────────────
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
    tf.keras.callbacks.CSVLogger(f"{SAVE_DIR}/log.csv", append=True),
]

print("\n--- 학습 시작 ---")
history = model.fit(
    train_ds,
    validation_data=val_ds,
    epochs=EPOCHS,
    steps_per_epoch=STEPS_PER_EPOCH,
    validation_steps=VALIDATION_STEPS,
    callbacks=callbacks,
    verbose=1,
)

# ── 7. Pure INT8 TFLite 변환 ─────────────────────────────────────────────────
print("\n=== Pure INT8 TFLite 변환 (Real Val Dataset Calibration) ===")

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
tflite_path = f"{SAVE_DIR}/brailledet_v1_pure_int8.tflite"

with open(tflite_path, "wb") as f:
    f.write(tflite_model)

print(f"\n🎉 변환 성공! Pure INT8 모델 저장 완료: {tflite_path}")
print(f"📦 파일 크기: {len(tflite_model)/1024:.1f} KB")


# ── 8. Train/Val 있음·없음 오판 지표 (TP/FP/FN/TN + 중심점 오차) ───────────────
def evaluate_presence(model, img_dirs, lbl_dirs, name="val", conf_thresh=0.5, sample_n=None):
    all_paths = []
    for img_dir, lbl_dir in zip(img_dirs, lbl_dirs):
        if not os.path.exists(img_dir):
            continue
        paths = sorted(sum([glob.glob(f"{img_dir}/{e}") for e in EXTS], []))
        for p in paths:
            lbl = f"{lbl_dir}/{Path(p).stem}.txt"
            all_paths.append((p, lbl))

    if sample_n is not None and len(all_paths) > sample_n:
        all_paths = random.sample(all_paths, sample_n)

    tp = fp = fn = tn = 0
    center_errors = []

    for img_path, lbl_path in all_paths:
        img = cv2.imread(str(img_path), cv2.IMREAD_GRAYSCALE)
        if img is None:
            continue
        img = cv2.resize(img, (IMG_SIZE, IMG_SIZE))
        inp = img.astype(np.float32)[np.newaxis, ..., np.newaxis] / 255.0

        pred = model.predict(inp, verbose=0)[0]
        conf_map = pred[..., 4]
        gy, gx = np.unravel_index(np.argmax(conf_map), conf_map.shape)
        conf = float(conf_map[gy, gx])
        pred_present = conf >= conf_thresh

        true_boxes = []
        if os.path.exists(lbl_path):
            with open(lbl_path) as f:
                for line in f:
                    parts = line.strip().split()
                    if len(parts) == 5:
                        true_boxes.append(list(map(float, parts[1:])))
        true_present = len(true_boxes) > 0

        if pred_present and true_present:
            tp += 1
            dx, dy, w, h = pred[gy, gx, 0:4]
            pred_cx = (gx + dx) / GRID_SIZE
            pred_cy = (gy + dy) / GRID_SIZE
            true_boxes.sort(key=lambda b: b[2] * b[3], reverse=True)
            tcx, tcy = true_boxes[0][0], true_boxes[0][1]
            center_errors.append(math.hypot(pred_cx - tcx, pred_cy - tcy))
        elif pred_present and not true_present:
            fp += 1
        elif not pred_present and true_present:
            fn += 1
        else:
            tn += 1

    total = tp + fp + fn + tn
    acc = (tp + tn) / max(total, 1)
    precision = tp / max(tp + fp, 1e-7)
    recall = tp / max(tp + fn, 1e-7)
    avg_center_err = np.mean(center_errors) if center_errors else float("nan")

    print(f"── {name} ({total}장) ──")
    print(f"  TP={tp}  FP={fp}(오탐: 없는데 있다함)  FN={fn}(미탐: 있는데 없다함)  TN={tn}")
    print(f"  accuracy={acc:.4f}  precision={precision:.4f}  recall={recall:.4f}")
    print(f"  중심점 평균오차(0~1 정규화)={avg_center_err:.4f}  (×{IMG_SIZE}px = {avg_center_err*IMG_SIZE:.1f}px)")
    return {"tp": tp, "fp": fp, "fn": fn, "tn": tn, "acc": acc,
            "precision": precision, "recall": recall, "center_err": avg_center_err}


evaluate_presence(model, [TRAIN_IMG, HARD_TRAIN_IMG], [TRAIN_LBL, HARD_TRAIN_LBL],
                   name="TRAIN", sample_n=500)
evaluate_presence(model, [VAL_IMG, HARD_VAL_IMG], [VAL_LBL, HARD_VAL_LBL],
                   name="VAL", sample_n=None)