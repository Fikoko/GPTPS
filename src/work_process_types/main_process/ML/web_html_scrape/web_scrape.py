

import requests
from bs4 import BeautifulSoup
import trafilatura
from extractnet import Extractor
from playwright.sync_api import sync_playwright
from urllib.parse import urljoin
import json
from datetime import datetime
import os

MIN_VISIBLE_TEXT_LENGTH = 500  # threshold for JS-heavy detection


# ------------------------------
# Utility: Detect JS-heavy pages
# ------------------------------
def is_js_heavy(html, min_text_length=MIN_VISIBLE_TEXT_LENGTH):
    """Detect if page is JS-heavy by checking visible text length & structure."""
    soup = BeautifulSoup(html, "html.parser")

    # Remove non-visible elements
    for tag in soup(["script", "style", "noscript"]):
        tag.decompose()

    visible_text = soup.get_text(separator=" ", strip=True)
    text_length = len(visible_text)

    script_ratio = len(soup.find_all("script")) / max(len(soup.find_all()), 1)

    # Check if main/article tags are empty
    main_content_tags = soup.find_all(["article", "main"])
    main_empty = all(len(tag.get_text(strip=True)) < 50 for tag in main_content_tags)

    return text_length < min_text_length or script_ratio > 0.3 or main_empty


# ------------------------------
# Fetch page: static or rendered
# ------------------------------
def get_rendered_html(url, browser, browser_type="chromium"):
    """Use headless browser to render JavaScript-heavy page."""
    page = browser.new_page()
    page.goto(url, wait_until="networkidle")
    content = page.content()
    page.close()
    return content


def fetch_page(url, browser=None, browser_type="chromium"):
    """Fetch page HTML, render if JS-heavy (browser optional)."""
    try:
        raw_html = requests.get(url, timeout=10).text
        if is_js_heavy(raw_html) and browser:
            print(f"Detected JS-heavy page – rendering with {browser_type}...")
            return get_rendered_html(url, browser, browser_type)
        else:
            print("Static page – using raw HTML.")
            return raw_html
    except requests.RequestException as e:
        print(f"Error fetching {url}: {e}")
        return ""


# ------------------------------
# Content extraction
# ------------------------------
def extract_main_content(html):
    """Extract main content from HTML using Trafilatura."""
    metadata = {}
    extracted_text = trafilatura.extract(html, include_comments=False, include_tables=False, with_metadata=True)

    if extracted_text:
        doc = trafilatura.extract(html, with_metadata=True, output_format="json")
        if doc:
            try:
                metadata = json.loads(doc)
            except json.JSONDecodeError:
                metadata = {}
        return extracted_text, metadata

    return "", metadata


def extract_structured_data(html):
    """Extract structured data from HTML using ExtractNet."""
    extractor = Extractor()
    return extractor.extract(html)


# ------------------------------
# News link extraction
# ------------------------------
def extract_news_links(url, html):
    """Extract news links from HTML using ExtractNet's ML model."""
    extractor = Extractor()
    structured_data = extractor.extract(html)
    news_links = []

    for key, value in structured_data.items():
        if isinstance(value, str) and value.startswith(("http://", "https://")):
            news_links.append(value)
        elif isinstance(value, list):
            for item in value:
                if isinstance(item, str) and item.startswith(("http://", "https://")):
                    news_links.append(item)
                elif isinstance(item, dict):
                    for subvalue in item.values():
                        if isinstance(subvalue, str) and subvalue.startswith(("http://", "https://")):
                            news_links.append(subvalue)

    # Convert relative URLs to absolute URLs
    news_links = [urljoin(url, link) for link in news_links]

    # Filter for likely news articles and deduplicate
    keywords = ("article", "news", "story", "post")
    news_links = list(set([link for link in news_links if any(k in link.lower() for k in keywords)]))

    return news_links


# ------------------------------
# Process a single news article
# ------------------------------
def process_news_link(url, browser, browser_type="chromium"):
    """Process a single news link to extract content and structured data."""
    print(f"Processing news article: {url}")
    html = fetch_page(url, browser, browser_type)
    if not html:
        return {"url": url, "text": "", "metadata": {}, "structured": {}}

    main_text, metadata = extract_main_content(html)
    structured_data = extract_structured_data(html)

    return {
        "url": url,
        "text": main_text,
        "metadata": metadata,
        "structured": structured_data
    }


# ------------------------------
# Main extraction pipeline
# ------------------------------
def extract_from_url(url: str, output_path: str, browser_type="chromium"):
    """Fetch URL, extract news links, process each link, and save to JSON."""
    start_time = datetime.now()
    print(f"\n[ALERT] Started processing {url} at {start_time.strftime('%Y-%m-%d %H:%M:%S')}")

    with sync_playwright() as p:
        browser_launcher = getattr(p, browser_type)
        browser = browser_launcher.launch(headless=True)

        # Fetch main page (detect JS-heavy)
        html = fetch_page(url, browser, browser_type)
        if not html:
            print(f"[ERROR] Failed to fetch main URL: {url}")
            browser.close()
            return

        # Extract news links
        news_links = extract_news_links(url, html)
        print(f"Found {len(news_links)} potential news links.")

        # Process each news link (per-article JS detection)
        articles = []
        for link in news_links:
            article_data = process_news_link(link, browser, browser_type)
            articles.append(article_data)

        browser.close()

    # Prepare output data
    output_data = {
        "main_url": url,
        "articles": articles,
        "timestamp": start_time.strftime("%Y-%m-%d %H:%M:%S")
    }

    # Save results to file
    try:
        os.makedirs(os.path.dirname(output_path), exist_ok=True)
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(output_data, f, ensure_ascii=False, indent=2)
        print(f"Results saved to {output_path}")
    except Exception as e:
        print(f"[ERROR] Failed to save results to {output_path}: {e}")

    end_time = datetime.now()
    print(f"[ALERT] Finished processing {url} at {end_time.strftime('%Y-%m-%d %H:%M:%S')} (Duration: {end_time - start_time})")


# ------------------------------
# Example usage
# ------------------------------
if __name__ == "__main__":
    test_url = "https://halktv.com.tr/gundem"
    output_path = "output/news_data.json"
    extract_from_url(test_url, output_path)
