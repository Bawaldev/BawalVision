import os
from icrawler.builtin import GoogleImageCrawler, BingImageCrawler

# List kata kunci spesifik i
queries = {
    "nasi": [
        "cooked white rice plate top view",
        "nasi putih matang piring",
        "steamed rice bowl"
    ],
    "ayam": [
        "cooked chicken dish plate",
        "ayam goreng matang piring",
        "roasted chicken piece"
    ],
    "bayam": [
        "cooked spinach dish",
        "tumis bayam matang",
        "sauteed spinach plate"
    ],
    "kangkung": [
        "tumis kangkung matang",
        "stir fried water spinach plate",
        "kangkung cah piring"
    ],
    "jeruk": [
        "fresh orange fruit whole and sliced",
        "buah jeruk segar piring"
    ],
    "pisang": [
        "ripe yellow banana fruit",
        "buah pisang matang kuning"
    ]
}

base_dir = "dataset_mbg_matang"
max_images_per_query = 200 

for category, search_terms in queries.items():
    category_dir = os.path.join(base_dir, category)
    os.makedirs(category_dir, exist_ok=True)
    
    for term in search_terms:
        print(f"\n[+] Downloading '{term}' into folder '{category}'...")
        
        # Menggunakan BingimageCrawler 
        crawler = BingImageCrawler(
            downloader_threads=4,
            storage={'root_dir': category_dir}
        )
        
        crawler.crawl(
            keyword=term,
            max_num=max_images_per_query,
            min_size=(200, 200)  # Filter ukuran minimal gambar 
        )

print("\n[SUCCESS] Selesai mengunduh seluruh dataset.")