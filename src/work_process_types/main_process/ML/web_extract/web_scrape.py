
# src/work_process_types/main_process/ML/web_html_scrape/web_scrape.py

import asyncio
from pathlib import Path
from typing import List, Dict
import tempfile
import subprocess

from extractnet import Extractor
from playwright.async_api import async_playwright

# -------------------------------
# ACHE Wrapper
# -------------------------------
class AcheExtractor:
    def __init__(self, ache_jar_path: str):
        self.ache_jar = ache_jar_path

    def extract(self, html_content: str) -> str:
        """Extract main content from HTML string using ACHE CLI."""
        with tempfile.NamedTemporaryFile("w+", suffix=".html", delete=False) as f:
            f.write(html_content)
            f.flush()
            file_path = f.name

        result = subprocess.run(
            ["java", "-jar", self.ache_jar, file_path],
            capture_output=True,
            text=True
        )

        Path(file_path).unlink(missing_ok=True)
        return result.stdout


# -------------------------------
# Web Scraper
# -------------------------------
class WebScraper:
    def __init__(self, ache_jar_path: str):
        self.ache = AcheExtractor(ache_jar_path)
        self.extractor = Extractor()

    async def fetch_page(self, url: str, js_render: bool = True) -> str:
        """Fetch page content, using Playwright if JS rendering is needed."""
        if not js_render:
            import requests
            resp = requests.get(url)
            resp.raise_for_status()
            return resp.text

        async with async_playwright() as p:
            browser = await p.chromium.launch(headless=True)
            page = await browser.new_page()
            await page.goto(url)
            content = await page.content()
            await browser.close()
            return content

    def extract_links(self, html: str) -> List[str]:
        """Extract all URLs from a page using ExtractNet."""
        return self.extractor.extract_links(html)

    def extract_main_content(self, html: str) -> str:
        """Use ACHE to extract main textual content."""
        return self.ache.extract(html)

    async def process_index_page(self, index_url: str, js_render: bool = True) -> Dict[str, str]:
        """Process an index page: fetch, extract links, fetch each page, extract content."""
        index_html = await self.fetch_page(index_url, js_render)
        links = self.extract_links(index_html)

        result = {}
        for link in links:
            try:
                page_html = await self.fetch_page(link, js_render)
                main_text = self.extract_main_content(page_html)
                result[link] = main_text
            except Exception as e:
                result[link] = f"Error: {e}"

        return result


# -------------------------------
# Helper function for synchronous use
# -------------------------------
def scrape_index_page(index_url: str, ache_jar_path: str, js_render: bool = True) -> Dict[str, str]:
    """Entry point for synchronous usage."""
    scraper = WebScraper(ache_jar_path)
    return asyncio.run(scraper.process_index_page(index_url, js_render))
