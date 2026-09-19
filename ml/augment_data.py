import os
import random
from PIL import Image
from torchvision import transforms

# Konfigurasi Target Train
TARGET_COUNT = 320
# Menggunakan raw string (r"") agar backslash Windows terbaca dengan benar
TRAIN_DIR = r"C:\Users\LENOVO\Documents\github\BawalVision\ml\AI1\dataset\train"

# 1. Kategori Hijauan (Fokus rotasi, hindari perubahan warna ekstrem)
transform_greenery = transforms.Compose([
    transforms.RandomRotation(degrees=45),
    transforms.RandomHorizontalFlip(p=0.5),
    transforms.ColorJitter(brightness=0.1, contrast=0.2, saturation=0.1),
])

# 2. Kategori Protein (Fokus variasi pantulan cahaya untuk ayam/tofu)
transform_protein = transforms.Compose([
    transforms.RandomRotation(degrees=30),
    transforms.RandomHorizontalFlip(p=0.5),
    transforms.RandomVerticalFlip(p=0.2),
    transforms.ColorJitter(brightness=0.3, contrast=0.3, saturation=0.2),
])

# 3. Kategori Standar (Buah dan Nasi)
transform_standard = transforms.Compose([
    transforms.RandomRotation(degrees=20),
    transforms.RandomHorizontalFlip(p=0.5),
    transforms.ColorJitter(brightness=0.2, contrast=0.1),
])

# Pemetaan Kelas ke Transformasi yang sesuai
class_transforms = {
    "bayam": transform_greenery,
    "kangkong": transform_greenery,
    "ayam": transform_protein,
    "tofu": transform_protein,
    "nasi": transform_standard,
    "jeruk": transform_standard,
    "pisang": transform_standard,
}

def augment_class(class_name, transform_pipeline):
    class_path = os.path.join(TRAIN_DIR, class_name)
    if not os.path.exists(class_path):
        print(f"[!] Folder tidak ditemukan: {class_path}")
        return

    # Ambil daftar gambar asli (mengabaikan file .gitkeep dan hasil augmentasi sebelumnya)
    valid_exts = ('.jpg', '.jpeg', '.png')
    original_images = [f for f in os.listdir(class_path) if f.lower().endswith(valid_exts) and not f.startswith("aug_")]
    
    # Hitung total gambar yang ada saat ini (asli + augmentasi jika sudah pernah dijalankan)
    total_current = len([f for f in os.listdir(class_path) if f.lower().endswith(valid_exts)])

    if total_current >= TARGET_COUNT or len(original_images) == 0:
        print(f"[=] {class_name}: Sudah memenuhi target atau tidak ada gambar asli ({total_current} gambar).")
        return

    needed = TARGET_COUNT - total_current
    print(f"[+] {class_name}: Punya {total_current}, butuh {needed} lagi. Memulai augmentasi...")

    # Generate gambar baru sampai memenuhi target 320
    for i in range(needed):
        rand_img_name = random.choice(original_images)
        img_path = os.path.join(class_path, rand_img_name)
        
        try:
            with Image.open(img_path) as img:
                img = img.convert("RGB")
                aug_img = transform_pipeline(img)
                
                # Simpan dengan penomoran berurut agar tidak tertimpa
                save_path = os.path.join(class_path, f"aug_{total_current + i}_{rand_img_name}")
                aug_img.save(save_path, "JPEG")
        except Exception as e:
            print(f"[!] Gagal memproses {img_path}: {e}")

print(f"=== Memulai Pemerataan Dataset Train (Target: {TARGET_COUNT} per kelas) ===")
for class_name in class_transforms.keys():
    augment_class(class_name, class_transforms[class_name])
print("=== Selesai! Semua kelas di folder train sudah mencapai 320 gambar. ===")