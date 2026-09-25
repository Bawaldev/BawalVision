import os
import time
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader
from torchvision import datasets, models, transforms
import onnxruntime as ort
import numpy as np



# 1.KONFIGURASI DIREKTORI & HYPERPARAMETER

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

DATA_DIR = os.path.join(BASE_DIR, "dataset")
WEIGHTS_DIR = os.path.join(BASE_DIR, "weights")
MODEL_DIR = os.path.join(BASE_DIR, "model")

for folder in [DATA_DIR, WEIGHTS_DIR, MODEL_DIR]:
    os.makedirs(folder, exist_ok=True)

PTH_PATH = os.path.join(WEIGHTS_DIR, "best_mobilenetv3.pth")
ONNX_PATH = os.path.join(MODEL_DIR, "ai1_vision.onnx")

BATCH_SIZE = 32
EPOCHS = 40
PATIENCE = 6  # Early stopping counter
NUM_CLASSES = 7

def main():
    os.makedirs(WEIGHTS_DIR, exist_ok=True)
    os.makedirs(MODEL_DIR, exist_ok=True)

    use_cuda = torch.cuda.is_available()
    device = torch.device("cuda" if use_cuda else "cpu")
    print(f"[+] Hardware aktif: {device}")

   
    # 2. PIPELINE PREPROCESSING
   
    data_transforms = transforms.Compose([
        transforms.Resize((224, 224)),
        transforms.ToTensor(),
        transforms.Normalize(mean=[0.485, 0.456, 0.406], std=[0.229, 0.224, 0.225]),
    ])

   
    # 3. DATA LOADER
   
    train_dataset = datasets.ImageFolder(
        os.path.join(DATA_DIR, "train"), transform=data_transforms
    )
    val_dataset = datasets.ImageFolder(
        os.path.join(DATA_DIR, "val"), transform=data_transforms
    )

    workers = 2 if os.name != "nt" else 0
    
    train_loader = DataLoader(
        train_dataset,
        batch_size=BATCH_SIZE,
        shuffle=True,
        num_workers=workers,
        pin_memory=use_cuda,
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=BATCH_SIZE,
        shuffle=False,
        num_workers=workers,
        pin_memory=use_cuda,
    )

    print(f"[+] Daftar Kelas ({len(train_dataset.classes)}): {train_dataset.classes}")
    print(f"[+] Total Sampel -> Train: {len(train_dataset)} | Val: {len(val_dataset)}")

   
    # 4. ARSITEKTUR MODEL (MobileNetV3-Small)
   
    model = models.mobilenet_v3_small(weights=models.MobileNet_V3_Small_Weights.DEFAULT)
    model.classifier[2] = nn.Dropout(p=0.5, inplace=True)
    in_features = model.classifier[3].in_features
    model.classifier[3] = nn.Linear(in_features, NUM_CLASSES)
    model = model.to(device)

    criterion = nn.CrossEntropyLoss()
    optimizer = optim.AdamW(model.parameters(), lr=1e-4, weight_decay=1e-3)
    scaler = torch.amp.GradScaler("cuda", enabled=use_cuda)

   
    # 5. LOOP PELATIHAN & EARLY STOPPING
   
    best_val_acc = 0.0
    epochs_no_improve = 0
    print("\n=== Memulai Training ===")
    
    start_time = time.time()  # <-- Mulai stopwatch

    for epoch in range(EPOCHS):
        model.train()
        running_loss, correct_train, total_train = 0.0, 0, 0

        # Fase Pelatihan (Train)
        for inputs, labels in train_loader:
            inputs, labels = inputs.to(device), labels.to(device)
            optimizer.zero_grad()

            with torch.amp.autocast("cuda", enabled=use_cuda):
                outputs = model(inputs)
                loss = criterion(outputs, labels)

            scaler.scale(loss).backward()
            scaler.step(optimizer)
            scaler.update()

            running_loss += loss.item() * inputs.size(0)
            _, preds = torch.max(outputs, 1)
            correct_train += torch.sum(preds == labels.data).item()
            total_train += labels.size(0)

        # Fase Evaluasi (Validasi)
        model.eval()
        correct_val, total_val = 0, 0
        with torch.no_grad():
            for inputs, labels in val_loader:
                inputs, labels = inputs.to(device), labels.to(device)
                outputs = model(inputs)
                _, preds = torch.max(outputs, 1)
                correct_val += torch.sum(preds == labels.data).item()
                total_val += labels.size(0)

        # Perhitungan Metrik
        train_acc = correct_train / total_train
        val_acc = correct_val / total_val

        print(
            f"Epoch [{epoch+1:02d}/{EPOCHS}] Loss: {running_loss/total_train:.4f} | "
            f"Train Acc: {train_acc:.4f} | Val Acc: {val_acc:.4f}"
        )

        # Evaluasi Early Stopping
        if val_acc > best_val_acc:
            best_val_acc = val_acc
            epochs_no_improve = 0
            torch.save(model.state_dict(), PTH_PATH)
            print(f"    -> Bobot terbaik disimpan di: {PTH_PATH} (Acc: {best_val_acc:.4f})")
        else:
            epochs_no_improve += 1
            print(f"    -> Early stopping counter: {epochs_no_improve}/{PATIENCE}")

        if epochs_no_improve >= PATIENCE:
            print(f"\n[!] Early stopping terpicu pada epoch {epoch+1}. Menghentikan pelatihan.")
            break

    # <-- Hentikan stopwatch
    time_elapsed = time.time() - start_time
    print(f"\n[⏱️] Total Waktu Training: {int(time_elapsed // 60)} menit {int(time_elapsed % 60)} detik")

   
    # 6. EKSPOR MODEL KE ONNX
   
    print("\n=== Mengonversi Bobot Terbaik ke ONNX ===")
    export_model = models.mobilenet_v3_small()
    export_model.classifier[2] = nn.Dropout(p=0.5, inplace=True)
    export_model.classifier[3] = nn.Linear(in_features, NUM_CLASSES)
    
    export_model.load_state_dict(torch.load(PTH_PATH, map_location="cpu", weights_only=True))
    export_model.eval()

    dummy_input = torch.randn(1, 3, 224, 224, requires_grad=False)
    
    torch.onnx.export(
        export_model,
        dummy_input,
        ONNX_PATH,
        export_params=True,
        opset_version=17,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["output"],
        dynamic_axes=None, 
    )
    print(f"[✓] File ONNX (FP32 siap eksekusi) tersimpan di: {ONNX_PATH}")

   
    # 7. DETAIL SANITY CHECK (PyTorch vs ONNX)
   
    print("\n=== Melakukan Sanity Check (Detail Perbandingan) ===")
    
    # 7.1. Inferensi PyTorch Asli
    with torch.no_grad():
        torch_output = export_model(dummy_input).cpu().numpy()

    # 7.2. Inferensi ONNX Runtime
    ort_session = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    ort_inputs = {ort_session.get_inputs()[0].name: dummy_input.cpu().numpy()}
    onnx_output = ort_session.run(None, ort_inputs)[0]

    # 7.3. Hitung Selisih Mutlak Maksimum
    max_diff = np.max(np.abs(torch_output - onnx_output))
    mean_diff = np.mean(np.abs(torch_output - onnx_output))

    print(f" -> Bentuk Output PyTorch : {torch_output.shape}")
    print(f" -> Bentuk Output ONNX    : {onnx_output.shape}")
    print(f" -> Max Absolute Diff     : {max_diff:.8f}")
    print(f" -> Mean Absolute Diff    : {mean_diff:.8f}")
    print("-" * 50)
    print("Contoh 7 Logit Mentah (PyTorch) :", np.round(torch_output[0], 4))
    print("Contoh 7 Logit Mentah (ONNX)    :", np.round(onnx_output[0], 4))
    print("-" * 50)

    try:
        np.testing.assert_allclose(torch_output, onnx_output, rtol=1e-03, atol=1e-05)
        print("[✓] Sanity check sukses! Grafik ONNX terbukti identik dengan PyTorch (selisih dalam batas toleransi aman).")
    except AssertionError as e:
        print("[!] Peringatan: Terdapat perbedaan margin kalkulasi antara PyTorch dan ONNX.")
        print(e)    

if __name__ == "__main__":
    main()