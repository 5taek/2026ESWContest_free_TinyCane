import os
import glob
import random
import unicodedata
from collections import Counter, defaultdict

import numpy as np
import cv2
import tensorflow as tf


# ============================================================
# 1. 설정
# ============================================================

SYNTH_DIR = "/content/synth_out_rec"
SYNTH_IMG_DIR = os.path.join(SYNTH_DIR, "images")
SYNTH_LABELS_TXT = os.path.join(SYNTH_DIR, "labels.txt")

REAL_DIR = "/content/drive/MyDrive/rec_v3/real/ok"
OUT_DIR = "/content/drive/MyDrive/rec_v3/model"

REC_H = 32
REC_W = 160

# ⭐ main.cpp가 검정(0)으로 패딩하는 것과 일치
PAD_VALUE = 0

MAX_LABEL_LEN = 6
BATCH_SIZE = 64
EPOCHS = 200

# 실사진을 class별로 몇 %를 TEST로 떼어낼지
REAL_TEST_RATIO = 0.20

# 한 batch에서 실사진 비율 (0.50 -> 합성 32 + 실사 32)
REAL_RATIO = 0.50

VAL_RATIO = 0.10
SEED = 42

os.makedirs(OUT_DIR, exist_ok=True)


# ============================================================
# 2. 문자 사전
# ============================================================

DIGITS = list("0123456789")
HANGUL = list("직행급순환남구동북서성수달칠곡가창팔공")
SYMBOLS = ["-"]

CHARSET = DIGITS + HANGUL + SYMBOLS

# Unicode NFC 고정
CHARSET = [unicodedata.normalize("NFC", c) for c in CHARSET]

# 0 = CTC blank
NUM_CLASSES = len(CHARSET) + 1

char2idx = {c: i + 1 for i, c in enumerate(CHARSET)}
idx2char = {i + 1: c for i, c in enumerate(CHARSET)}
idx2char[0] = ""

print("=" * 70)
print("문자 사전")
print("=" * 70)
print("CHARSET =", CHARSET)
print("문자 수 =", len(CHARSET))
print("NUM_CLASSES =", NUM_CLASSES)
print()


# ============================================================
# 3. Label encode
# ============================================================

def normalize_label(text):
    return unicodedata.normalize("NFC", str(text).strip())


def encode(text):
    text = normalize_label(text)
    seq = [char2idx[c] for c in text if c in char2idx]
    seq = seq[:MAX_LABEL_LEN]
    return np.array(seq + [0] * (MAX_LABEL_LEN - len(seq)), dtype=np.int32)


# ============================================================
# 4. Image normalize
# ============================================================

def normalize(img):
    # uint8 [0,255] -> float32 [-1,1]
    return (img.astype(np.float32) / 255.0 - 0.5) / 0.5


# ============================================================
# 5. 이미지 로드
#
# ⭐ fallback 리사이즈를 종횡비 유지 + 검정 패딩으로 교체.
#    main.cpp의 bus_prepare_rec_input()과 동일한 방식
#    (스트레치 아님, 항상 높이를 32에 맞추고 왼쪽 정렬 + 검정 패딩).
#    정상 파이프라인이면 이미 32x160이라 이 분기를 안 타지만,
#    혹시 다른 크기 이미지가 섞여도 안전하게 처리하기 위한 안전장치.
# ============================================================

def to_board_format(img):
    h, w = img.shape[:2]
    scale = REC_H / h
    new_w = max(1, min(REC_W, int(round(w * scale))))

    resized = cv2.resize(img, (new_w, REC_H), interpolation=cv2.INTER_AREA)

    out = np.full((REC_H, REC_W), PAD_VALUE, dtype=np.uint8)
    # 왼쪽 정렬 (main.cpp가 ox=0부터 채우는 것과 동일)
    out[:, :new_w] = resized

    return out


def load_gray_32x160(path):
    img = cv2.imread(path, cv2.IMREAD_GRAYSCALE)
    if img is None:
        return None

    if img.shape != (REC_H, REC_W):
        img = to_board_format(img)

    return img


# ============================================================
# 6. 합성 데이터 로드
# ============================================================

def load_synth():
    print("=" * 70)
    print("[합성 데이터 로드]")
    print("=" * 70)

    pairs = []
    with open(SYNTH_LABELS_TXT, "r", encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue

            parts = line.split("\t")
            if len(parts) != 2:
                parts = line.split()
            if len(parts) != 2:
                continue

            fname = parts[0].strip()
            label = normalize_label(parts[1])
            pairs.append((fname, label))

    images = []
    labels = []
    skipped = []

    for fname, label in pairs:
        invalid_chars = [c for c in label if c not in char2idx]

        if not label or invalid_chars:
            skipped.append((fname, label, invalid_chars))
            continue

        path = os.path.join(SYNTH_IMG_DIR, fname)
        img = load_gray_32x160(path)

        if img is None:
            skipped.append((fname, "이미지 로드 실패", []))
            continue

        images.append(img)
        labels.append(label)

    images = np.array(images, dtype=np.uint8)

    print(f"합성 전체: {len(images)}장")
    print(f"합성 스킵: {len(skipped)}장")

    print("\n[합성 라벨 분포]")
    counter = Counter(labels)
    for label, count in counter.most_common():
        print(f"  {label:8s}: {count:5d}")

    if skipped:
        print("\n[합성 스킵 예시]")
        for item in skipped[:10]:
            print(" ", item)

    # --------------------------------------------------------
    # 합성 90/10 split
    # --------------------------------------------------------
    rng = np.random.default_rng(SEED)
    indices = rng.permutation(len(images))

    n_val = max(1, int(len(images) * VAL_RATIO))
    val_idx = indices[:n_val]
    train_idx = indices[n_val:]

    train_imgs = images[train_idx]
    train_labels = [labels[i] for i in train_idx]

    val_imgs = images[val_idx]
    val_labels = [labels[i] for i in val_idx]

    print(f"\n합성 TRAIN: {len(train_imgs)}장")
    print(f"합성 VAL:   {len(val_imgs)}장")

    return train_imgs, train_labels, val_imgs, val_labels


# ============================================================
# 7. 실사진 로드
# ============================================================

def load_real():
    print("\n" + "=" * 70)
    print("[실사진 로드]")
    print("=" * 70)

    paths = []
    for ext in ("*.png", "*.jpg", "*.jpeg", "*.PNG", "*.JPG", "*.JPEG"):
        paths.extend(glob.glob(os.path.join(REAL_DIR, ext)))
    paths = sorted(set(paths))

    if not paths:
        raise FileNotFoundError("실사진을 찾지 못했습니다:\n" + REAL_DIR)

    images = []
    labels = []
    filenames = []
    skipped = []

    for path in paths:
        filename = os.path.basename(path)
        stem = os.path.splitext(filename)[0]

        if "_" not in stem:
            skipped.append((filename, "파일명에 '_' 없음"))
            continue

        # 마지막 '_' 앞까지를 라벨로 사용
        # 북구2_220.png -> 북구2 / 410_001.png -> 410 / 410-1_001.png -> 410-1
        label = normalize_label(stem.rsplit("_", 1)[0])
        invalid_chars = [c for c in label if c not in char2idx]

        if not label:
            skipped.append((filename, "빈 라벨"))
            continue

        if invalid_chars:
            skipped.append((filename, "알 수 없는 문자: " + repr(invalid_chars)))
            continue

        img = load_gray_32x160(path)
        if img is None:
            skipped.append((filename, "이미지 로드 실패"))
            continue

        images.append(img)
        labels.append(label)
        filenames.append(filename)

    images = np.array(images, dtype=np.uint8)

    print(f"실제 파일: {len(paths)}장")
    print(f"정상 로드: {len(images)}장")
    print(f"스킵:      {len(skipped)}장")

    print("\n[실사진 전체 라벨 분포]")
    counter = Counter(labels)
    for label, count in counter.most_common():
        print(f"  {label:8s}: {count:5d}")

    print("\n[핵심 라벨 확인]")
    for target in ("410", "410-1", "503", "937", "북구2"):
        print(f"  {target:8s}: {counter.get(target, 0):5d}장")

    if skipped:
        print("\n[스킵 원인 TOP 20]")
        for item in skipped[:20]:
            print(" ", item)

    return images, labels, filenames


# ============================================================
# 8. 실사진 class-balanced TRAIN / TEST split
# ============================================================

def split_real_data(images, labels, filenames):
    print("\n" + "=" * 70)
    print("[실사진 TRAIN / TEST 분할]")
    print("=" * 70)

    by_label = defaultdict(list)
    for i, label in enumerate(labels):
        by_label[label].append(i)

    rng = random.Random(SEED)

    train_indices = []
    test_indices = []

    for label in sorted(by_label.keys()):
        idxs = by_label[label].copy()
        rng.shuffle(idxs)

        # 최소 1장은 TEST
        n_test = max(1, int(round(len(idxs) * REAL_TEST_RATIO)))

        # 전체가 1장인 경우 제외
        if len(idxs) > 1:
            n_test = min(n_test, len(idxs) - 1)
        else:
            n_test = 0

        test_part = idxs[:n_test]
        train_part = idxs[n_test:]

        train_indices.extend(train_part)
        test_indices.extend(test_part)

        print(
            f"  {label:8s}: 전체={len(idxs):3d} "
            f"TRAIN={len(train_part):3d} TEST={len(test_part):3d}"
        )

    train_indices = np.array(train_indices, dtype=np.int32)
    test_indices = np.array(test_indices, dtype=np.int32)

    np.random.default_rng(SEED).shuffle(train_indices)
    np.random.default_rng(SEED + 1).shuffle(test_indices)

    train_imgs = images[train_indices]
    train_labels = [labels[i] for i in train_indices]
    train_filenames = [filenames[i] for i in train_indices]

    test_imgs = images[test_indices]
    test_labels = [labels[i] for i in test_indices]
    test_filenames = [filenames[i] for i in test_indices]

    print(f"\n실사진 TRAIN: {len(train_imgs)}장")
    print(f"실사진 TEST:  {len(test_imgs)}장")

    print("\n[실사진 TRAIN 분포]")
    for label, count in Counter(train_labels).most_common():
        print(f"  {label:8s}: {count:5d}")

    print("\n[실사진 TEST 분포]")
    for label, count in Counter(test_labels).most_common():
        print(f"  {label:8s}: {count:5d}")

    return (
        train_imgs,
        train_labels,
        train_filenames,
        test_imgs,
        test_labels,
        test_filenames,
    )


# ============================================================
# 9. 실사진 augmentation
#
# ⭐ translation 증강의 패딩을 회색(127) -> 검정(0)으로 통일
#    (보드/합성생성기/실사변환기와 일치)
# ============================================================

def augment_real(img):
    out = img.copy()

    # brightness / contrast
    if random.random() < 0.70:
        alpha = random.uniform(0.75, 1.30)
        beta = random.uniform(-25, 25)
        out = np.clip(out.astype(np.float32) * alpha + beta, 0, 255).astype(np.uint8)

    # gaussian blur
    if random.random() < 0.30:
        out = cv2.GaussianBlur(out, (3, 3), 0)

    # gaussian noise
    if random.random() < 0.40:
        noise = np.random.normal(0, random.uniform(2, 8), out.shape)
        out = np.clip(out.astype(np.float32) + noise, 0, 255).astype(np.uint8)

    # 작은 translation
    if random.random() < 0.40:
        dx = random.randint(-3, 3)
        dy = random.randint(-1, 1)
        M = np.float32([[1, 0, dx], [0, 1, dy]])
        out = cv2.warpAffine(out, M, (REC_W, REC_H), borderValue=PAD_VALUE)

    return out


# ============================================================
# 10. Mixed Training Sequence
#
# batch 64 기준: 합성 32 + 실사 32
# 실사 32장은 클래스 균형으로 선택한다.
# (410 / 410-1 / 503 / 937 / 북구2 등 데이터 수가 동일하지 않아도
#  특정 클래스가 batch를 독점하지 않는다.)
# ============================================================

class MixedSequence(tf.keras.utils.Sequence):
    def __init__(
        self,
        synth_imgs,
        synth_labels,
        real_imgs,
        real_labels,
        batch_size,
        real_ratio,
        steps,
    ):
        super().__init__()

        self.synth_imgs = synth_imgs
        self.synth_labels = synth_labels
        self.real_imgs = real_imgs
        self.real_labels = real_labels
        self.batch_size = batch_size

        self.n_real = int(round(batch_size * real_ratio))
        self.n_synth = batch_size - self.n_real
        self.steps = steps

        self.real_by_label = defaultdict(list)
        for i, label in enumerate(real_labels):
            self.real_by_label[label].append(i)
        self.real_classes = sorted(self.real_by_label.keys())

        print("\n" + "=" * 70)
        print("[실사진 Balanced Sampling]")
        print("=" * 70)
        for label in self.real_classes:
            print(f"  {label:8s}: {len(self.real_by_label[label]):4d}장")

        print(f"\n전체 batch: {batch_size}")
        print(f"합성 batch: {self.n_synth}")
        print(f"실사 batch: {self.n_real}")

    def __len__(self):
        return self.steps

    def __getitem__(self, index):
        xs = []
        ys = []

        # 합성
        if self.n_synth > 0:
            synth_indices = np.random.randint(
                0, len(self.synth_imgs), size=self.n_synth
            )
            for i in synth_indices:
                xs.append(self.synth_imgs[i])
                ys.append(encode(self.synth_labels[i]))

        # 실사 (클래스 균등 선택)
        if self.n_real > 0 and self.real_classes:
            chosen_classes = np.random.choice(
                self.real_classes, size=self.n_real, replace=True
            )
            for label in chosen_classes:
                i = random.choice(self.real_by_label[label])
                img = augment_real(self.real_imgs[i])
                xs.append(img)
                ys.append(encode(label))

        x = normalize(np.array(xs, dtype=np.uint8))[..., np.newaxis]
        y = np.array(ys, dtype=np.int32)

        # 합성/실사 순서에 의한 편향 방지
        perm = np.random.permutation(len(x))

        return x[perm], y[perm]


# ============================================================
# 11. Validation Dataset
#
# validation은 합성 VAL만 사용.
# 실사진 TEST는 학습 중 절대 보지 않는다.
# ============================================================

def make_dataset(images, labels, batch_size):
    x = normalize(images)[..., np.newaxis]
    y = np.array([encode(label) for label in labels], dtype=np.int32)

    ds = tf.data.Dataset.from_tensor_slices((x, y))
    return ds.batch(batch_size).prefetch(tf.data.AUTOTUNE)


# ============================================================
# 12. Model
# ============================================================

def build_model():
    inp = tf.keras.Input(shape=(REC_H, REC_W, 1), name="image")

    def dw_block(x, ch, k=3):
        x = tf.keras.layers.DepthwiseConv2D(k, padding="same", use_bias=False)(x)
        x = tf.keras.layers.BatchNormalization()(x)
        x = tf.keras.layers.ReLU()(x)

        x = tf.keras.layers.Conv2D(ch, 1, padding="same", use_bias=False)(x)
        x = tf.keras.layers.BatchNormalization()(x)
        x = tf.keras.layers.ReLU()(x)

        return x

    # 32 x 160
    x = tf.keras.layers.Conv2D(32, 3, padding="same", use_bias=False)(inp)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU()(x)
    x = tf.keras.layers.MaxPooling2D((2, 2))(x)

    # 16 x 80
    x = dw_block(x, 64, k=5)
    x = tf.keras.layers.MaxPooling2D((2, 2))(x)

    # 8 x 40
    x = dw_block(x, 96, k=5)
    x = tf.keras.layers.MaxPooling2D((2, 1))(x)

    # 4 x 40
    x = dw_block(x, 128, k=3)
    x = dw_block(x, 128, k=3)
    x = tf.keras.layers.MaxPooling2D((2, 1))(x)

    # 2 x 40
    x = tf.keras.layers.Conv2D(192, (2, 1), padding="valid", use_bias=False)(x)
    x = tf.keras.layers.BatchNormalization()(x)
    x = tf.keras.layers.ReLU()(x)

    # 1 x 40 x 192
    x = tf.keras.layers.Conv2D(NUM_CLASSES, 1, padding="same")(x)

    # 40 x NUM_CLASSES
    out = tf.keras.layers.Reshape((-1, NUM_CLASSES), name="logits")(x)

    model = tf.keras.Model(inp, out, name="bus_rec_v4_synth_real")

    return model


# ============================================================
# 13. CTC loss
# ============================================================

def ctc_loss_fn(y_true, y_pred):
    y_true = tf.cast(y_true, tf.int32)

    label_length = tf.reduce_sum(tf.cast(y_true > 0, tf.int32), axis=-1)

    batch_size = tf.shape(y_pred)[0]
    logit_length = tf.fill([batch_size], tf.shape(y_pred)[1])

    loss = tf.nn.ctc_loss(
        labels=y_true,
        logits=y_pred,
        label_length=label_length,
        logit_length=logit_length,
        logits_time_major=False,
        blank_index=0,
    )

    return tf.reduce_mean(loss)


# ============================================================
# 14. Greedy CTC decode
# ============================================================

def greedy_decode(logits):
    indices = np.argmax(logits, axis=-1)

    output = []
    prev = -1

    for index in indices:
        index = int(index)

        # CTC: 같은 문자의 연속은 하나로 합치고 blank(0)는 제거
        if index != prev and index != 0:
            output.append(index)

        prev = index

    return "".join(idx2char.get(index, "?") for index in output)


# ============================================================
# 15. Keras 평가
# ============================================================

def evaluate_keras(model, images, labels, tag, show_wrong=20, show_all=False):
    if len(images) == 0:
        print(f"\n[{tag}] 데이터 없음")
        return 0.0

    x = normalize(images)[..., np.newaxis]
    predictions = model.predict(x, verbose=0)

    correct = 0
    wrong = []
    pred_counter = Counter()

    for i, label in enumerate(labels):
        pred = greedy_decode(predictions[i])
        pred_counter[pred] += 1

        if pred == label:
            correct += 1
        else:
            wrong.append((i, label, pred))

    accuracy = correct / len(labels)

    print("\n" + "=" * 70)
    print(f"[{tag}]")
    print(f"정확도: {accuracy:.4f} ({correct}/{len(labels)})")

    print("\n예측 분포:")
    for pred, count in pred_counter.most_common():
        print(f"  '{pred}': {count}개")

    if show_all:
        print("\n전체 개별 예측:")
        for i, label in enumerate(labels):
            pred = greedy_decode(predictions[i])
            mark = "✅" if pred == label else "❌"
            print(f"{mark} [{i:03d}] 정답={label:8s} 예측={pred}")
    else:
        print("\n오답:")
        for i, label, pred in wrong[:show_wrong]:
            print(f"  [{i:03d}] 정답='{label}' → 예측='{pred}'")

        if len(wrong) > show_wrong:
            print(f"  ... {len(wrong) - show_wrong}개")

    return accuracy


# ============================================================
# 16. Per-class accuracy
#
# 전체 정확도만 보면 북구2/410 등이 섞여서 감춰질 수 있어서
# 클래스별 정확도를 따로 출력한다.
# ============================================================

def evaluate_keras_per_class(model, images, labels, tag):
    if len(images) == 0:
        return

    x = normalize(images)[..., np.newaxis]
    predictions = model.predict(x, verbose=0)

    total = Counter()
    correct = Counter()

    for i, label in enumerate(labels):
        pred = greedy_decode(predictions[i])
        total[label] += 1
        if pred == label:
            correct[label] += 1

    print("\n" + "=" * 70)
    print(f"[{tag} 클래스별 정확도]")

    for label in sorted(total.keys()):
        acc = correct[label] / total[label]
        print(f"  {label:8s}: {acc:.4f} ({correct[label]}/{total[label]})")


# ============================================================
# 17. TFLite 변환
#
# representative dataset: 합성 TRAIN 일부 + 실사 TRAIN 전체
# 실사 TEST는 절대 사용하지 않는다.
# ============================================================

def convert_tflite(model, synth_train_imgs, real_train_imgs):
    print("\n[Representative Dataset]")

    parts = []

    if len(synth_train_imgs):
        n_synth = min(300, len(synth_train_imgs))
        synth_idx = np.random.choice(
            len(synth_train_imgs), size=n_synth, replace=False
        )
        parts.append(synth_train_imgs[synth_idx])

    if len(real_train_imgs):
        n_real = min(300, len(real_train_imgs))
        real_idx = np.random.choice(len(real_train_imgs), size=n_real, replace=False)
        parts.append(real_train_imgs[real_idx])

    rep_pool = np.concatenate(parts, axis=0)
    print(f"Representative pool: {len(rep_pool)}장")

    def representative_dataset():
        indices = np.arange(len(rep_pool))
        np.random.shuffle(indices)

        for i in indices:
            x = normalize(rep_pool[i])[np.newaxis, ..., np.newaxis]
            yield [x.astype(np.float32)]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]

    # ESP에서 기존 코드와 맞추기 위해 input/output은 float32로 유지
    converter.inference_input_type = tf.float32
    converter.inference_output_type = tf.float32

    return converter.convert()


# ============================================================
# 18. TFLite 평가
# ============================================================

def evaluate_tflite(tflite_bytes, images, labels, tag, show_wrong=20, show_all=False):
    if len(images) == 0:
        print(f"\n[{tag}] 데이터 없음")
        return 0.0

    interpreter = tf.lite.Interpreter(model_content=tflite_bytes)
    interpreter.allocate_tensors()

    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    print("\n" + "=" * 70)
    print(f"[{tag}]")
    print("Input:", input_details[0]["shape"], input_details[0]["dtype"])
    print("Output:", output_details[0]["shape"], output_details[0]["dtype"])

    correct = 0
    wrong = []
    pred_counter = Counter()
    results = []

    for i, label in enumerate(labels):
        x = normalize(images[i])[np.newaxis, ..., np.newaxis].astype(np.float32)

        interpreter.set_tensor(input_details[0]["index"], x)
        interpreter.invoke()

        output = interpreter.get_tensor(output_details[0]["index"])[0]

        # 혹시 output이 int8인 모델이어도 대응
        if output_details[0]["dtype"] == np.int8:
            scale, zero_point = output_details[0]["quantization"]
            if scale > 0:
                output = (output.astype(np.float32) - zero_point) * scale

        pred = greedy_decode(output)
        pred_counter[pred] += 1
        results.append((i, label, pred))

        if pred == label:
            correct += 1
        else:
            wrong.append((i, label, pred))

    accuracy = correct / len(labels)

    print(f"\n정확도: {accuracy:.4f} ({correct}/{len(labels)})")

    print("\n예측 분포:")
    for pred, count in pred_counter.most_common():
        print(f"  '{pred}': {count}개")

    if show_all:
        print("\n전체 개별 예측:")
        for i, label, pred in results:
            mark = "✅" if pred == label else "❌"
            print(f"{mark} [{i:03d}] 정답={label:8s} 예측={pred}")
    else:
        print("\n오답:")
        for i, label, pred in wrong[:show_wrong]:
            print(f"  [{i:03d}] 정답='{label}' → 예측='{pred}'")

        if len(wrong) > show_wrong:
            print(f"  ... {len(wrong) - show_wrong}개")

    return accuracy


# ============================================================
# 19. TFLite 클래스별 정확도
# ============================================================

def evaluate_tflite_per_class(tflite_bytes, images, labels, tag):
    if len(images) == 0:
        return

    interpreter = tf.lite.Interpreter(model_content=tflite_bytes)
    interpreter.allocate_tensors()

    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    total = Counter()
    correct = Counter()

    for i, label in enumerate(labels):
        x = normalize(images[i])[np.newaxis, ..., np.newaxis].astype(np.float32)

        interpreter.set_tensor(input_details[0]["index"], x)
        interpreter.invoke()

        output = interpreter.get_tensor(output_details[0]["index"])[0]

        if output_details[0]["dtype"] == np.int8:
            scale, zero_point = output_details[0]["quantization"]
            if scale > 0:
                output = (output.astype(np.float32) - zero_point) * scale

        pred = greedy_decode(output)

        total[label] += 1
        if pred == label:
            correct[label] += 1

    print("\n" + "=" * 70)
    print(f"[{tag} TFLite 클래스별 정확도]")

    for label in sorted(total.keys()):
        acc = correct[label] / total[label]
        print(f"  {label:8s}: {acc:.4f} ({correct[label]}/{total[label]})")


# ============================================================
# 20. Learning Rate
# ============================================================

def make_lr_schedule(steps_per_epoch):
    total_steps = steps_per_epoch * EPOCHS
    warmup_steps = steps_per_epoch * 5

    cosine = tf.keras.optimizers.schedules.CosineDecay(
        initial_learning_rate=1e-3,
        decay_steps=max(1, total_steps - warmup_steps),
        alpha=1e-5,
    )

    class WarmupCosine(tf.keras.optimizers.schedules.LearningRateSchedule):
        def __init__(self, warmup_steps, cosine_schedule):
            self.warmup_steps = warmup_steps
            self.cosine_schedule = cosine_schedule

        def __call__(self, step):
            step = tf.cast(step, tf.float32)
            warmup = tf.cast(self.warmup_steps, tf.float32)

            warmup_lr = 1e-4 + (1e-3 - 1e-4) * step / tf.maximum(warmup, 1.0)
            cosine_lr = self.cosine_schedule(tf.maximum(step - warmup, 0))

            return tf.where(step < warmup, warmup_lr, cosine_lr)

        def get_config(self):
            return {}

    return WarmupCosine(warmup_steps, cosine)


# ============================================================
# 21. Main
# ============================================================

def main():
    # seed
    random.seed(SEED)
    np.random.seed(SEED)
    tf.random.set_seed(SEED)

    # --------------------------------------------------------
    # 데이터 로드
    # --------------------------------------------------------
    synth_train_imgs, synth_train_labels, synth_val_imgs, synth_val_labels = (
        load_synth()
    )

    real_imgs, real_labels, real_filenames = load_real()

    (
        real_train_imgs,
        real_train_labels,
        real_train_filenames,
        real_test_imgs,
        real_test_labels,
        real_test_filenames,
    ) = split_real_data(real_imgs, real_labels, real_filenames)

    # --------------------------------------------------------
    # 최종 데이터 확인
    # --------------------------------------------------------
    print("\n" + "#" * 70)
    print("# 최종 데이터 구성")
    print("#" * 70)

    print(f"합성 TRAIN : {len(synth_train_imgs)}")
    print(f"합성 VAL   : {len(synth_val_imgs)}")
    print(f"실사 TRAIN : {len(real_train_imgs)}")
    print(f"실사 TEST  : {len(real_test_imgs)}")

    # --------------------------------------------------------
    # 모델
    # --------------------------------------------------------
    model = build_model()

    print("\n" + "=" * 70)
    print("MODEL")
    model.summary()

    print(f"\n파라미터: {model.count_params():,}")
    print(f"출력 shape: {model.output_shape}")

    # --------------------------------------------------------
    # steps
    # --------------------------------------------------------
    steps_per_epoch = max(50, len(synth_train_imgs) // BATCH_SIZE)
    print(f"\nsteps_per_epoch: {steps_per_epoch}")

    # --------------------------------------------------------
    # compile
    # --------------------------------------------------------
    optimizer = tf.keras.optimizers.Adam(make_lr_schedule(steps_per_epoch))
    model.compile(optimizer=optimizer, loss=ctc_loss_fn)

    # --------------------------------------------------------
    # Mixed sequence
    # --------------------------------------------------------
    train_sequence = MixedSequence(
        synth_train_imgs,
        synth_train_labels,
        real_train_imgs,
        real_train_labels,
        BATCH_SIZE,
        REAL_RATIO,
        steps_per_epoch,
    )

    # --------------------------------------------------------
    # Validation (합성 VAL만 사용, 실사 TEST는 여기 들어가지 않는다)
    # --------------------------------------------------------
    val_dataset = make_dataset(synth_val_imgs, synth_val_labels, BATCH_SIZE)

    # --------------------------------------------------------
    # callbacks
    # --------------------------------------------------------
    best_path = os.path.join(OUT_DIR, "best_synth_real_v4.weights.h5")

    callbacks = [
        tf.keras.callbacks.ModelCheckpoint(
            filepath=best_path,
            monitor="val_loss",
            save_best_only=True,
            save_weights_only=True,
            verbose=1,
        ),
        tf.keras.callbacks.EarlyStopping(
            monitor="val_loss",
            patience=20,
            restore_best_weights=True,
            verbose=1,
        ),
    ]

    # --------------------------------------------------------
    # 학습
    # --------------------------------------------------------
    print("\n" + "=" * 70)
    print("학습 시작")
    print("합성 + 실사 TRAIN")
    print(f"배치 구성: 합성 {train_sequence.n_synth} + 실사 {train_sequence.n_real}")
    print("실사 TEST는 학습에 절대 사용하지 않음")
    print("=" * 70)

    model.fit(
        train_sequence,
        validation_data=val_dataset,
        epochs=EPOCHS,
        callbacks=callbacks,
        verbose=1,
    )

    # ========================================================
    # Keras 평가
    # ========================================================
    print("\n" + "#" * 70)
    print("# KERAS 평가")
    print("#" * 70)

    evaluate_keras(
        model, synth_val_imgs, synth_val_labels, tag="Keras 합성 VAL", show_wrong=20
    )

    evaluate_keras(
        model,
        real_train_imgs,
        real_train_labels,
        tag="Keras 실사 TRAIN",
        show_wrong=20,
    )

    evaluate_keras(
        model,
        real_test_imgs,
        real_test_labels,
        tag="Keras 실사 TEST ★★★",
        show_wrong=50,
        show_all=True,
    )

    evaluate_keras_per_class(
        model, real_test_imgs, real_test_labels, tag="Keras 실사 TEST"
    )

    # ========================================================
    # Keras 저장
    # ========================================================
    keras_path = os.path.join(OUT_DIR, "bus_rec_synth_real_v4.keras")
    model.save(keras_path)

    print("\nKeras 저장:")
    print(keras_path)

    # ========================================================
    # TFLite
    # ========================================================
    print("\n" + "#" * 70)
    print("# TFLITE INT8 변환")
    print("#" * 70)

    tflite_bytes = convert_tflite(model, synth_train_imgs, real_train_imgs)

    tflite_path = os.path.join(OUT_DIR, "bus_rec_synth_real_v4_int8.tflite")
    with open(tflite_path, "wb") as f:
        f.write(tflite_bytes)

    print("\nTFLite 저장:")
    print(tflite_path)
    print(f"크기: {len(tflite_bytes) / 1024:.1f} KB")

    # ========================================================
    # TFLite 평가
    # ========================================================
    print("\n" + "#" * 70)
    print("# TFLITE 평가")
    print("#" * 70)

    evaluate_tflite(
        tflite_bytes,
        synth_val_imgs,
        synth_val_labels,
        tag="TFLite 합성 VAL",
        show_wrong=20,
    )

    evaluate_tflite(
        tflite_bytes,
        real_train_imgs,
        real_train_labels,
        tag="TFLite 실사 TRAIN",
        show_wrong=20,
    )

    evaluate_tflite(
        tflite_bytes,
        real_test_imgs,
        real_test_labels,
        tag="TFLite 실사 TEST ★★★",
        show_wrong=50,
        show_all=True,
    )

    evaluate_tflite_per_class(
        tflite_bytes, real_test_imgs, real_test_labels, tag="TFLite 실사 TEST"
    )

    # ========================================================
    # 최종 요약
    # ========================================================
    print("\n" + "=" * 70)
    print("🎉 전체 완료")
    print("=" * 70)

    print("\n[데이터]")
    print(f"  합성 TRAIN : {len(synth_train_imgs)}")
    print(f"  합성 VAL   : {len(synth_val_imgs)}")
    print(f"  실사 TRAIN : {len(real_train_imgs)}")
    print(f"  실사 TEST  : {len(real_test_imgs)}")

    print("\n[실사 전체 라벨]")
    for label, count in Counter(real_labels).most_common():
        print(f"  {label:8s}: {count}")

    print("\n[생성 파일]")
    print(f"  Keras  : {keras_path}")
    print(f"  TFLite : {tflite_path}")


# ============================================================
# 22. 실행
# ============================================================

if __name__ == "__main__":
    main()