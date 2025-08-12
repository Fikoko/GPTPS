from transformers import AutoTokenizer, AutoModelForCausalLM, BitsAndBytesConfig
import torch
from langchain.text_splitter import RecursiveCharacterTextSplitter

# === Configuration ===

model_id = "microsoft/Phi-4-mini-instruct"
max_chunk_tokens = 4096
quantized = True

# === Load tokenizer & model once ===

tokenizer = AutoTokenizer.from_pretrained(model_id, trust_remote_code=True)
tokenizer.model_max_length = 128_000  # To avoid truncation warnings

bnb_config = BitsAndBytesConfig(
    load_in_4bit=True,
    bnb_4bit_use_double_quant=True,
    bnb_4bit_compute_dtype=torch.float16,
    bnb_4bit_quant_type="nf4"
) if quantized else None

model = AutoModelForCausalLM.from_pretrained(
    model_id,
    trust_remote_code=True,
    quantization_config=bnb_config,
    device_map="auto",
    torch_dtype=torch.float16
)

# === Utility functions ===

def split_text_into_chunks(text, max_tokens=max_chunk_tokens):
    splitter = RecursiveCharacterTextSplitter(
        chunk_size=max_tokens,
        chunk_overlap=max_tokens // 8,
        length_function=lambda x: len(tokenizer.encode(x, add_special_tokens=False)),
    )
    return splitter.split_text(text)

def generate_chunk(prompt, max_new_tokens=None):
    inputs = tokenizer(prompt, return_tensors="pt", truncation=True).to(model.device)
    generate_kwargs = dict(
        **inputs,
        do_sample=False,
        pad_token_id=tokenizer.eos_token_id,
        eos_token_id=tokenizer.eos_token_id
    )
    if max_new_tokens is not None:
        generate_kwargs["max_new_tokens"] = max_new_tokens
    else:
        generate_kwargs["max_new_tokens"] = 2048  # Large limit, let model decide length

    outputs = model.generate(**generate_kwargs)
    response = tokenizer.decode(outputs[0], skip_special_tokens=True)
    return response[len(prompt):].strip()

# === Main function ===

def llm_process(large_text: str, second_text: str) -> str:
    """
    Ingest large_text via chunk summarization, then generate output based on
    second_text prompt combined with the summary.

    Args:
        large_text (str): Large text input.
        second_text (str): Additional prompt/instructions/questions.

    Returns:
        str: Model-generated output text.
    """
    chunks = split_text_into_chunks(large_text)
    chunk_summaries = []
    for chunk in chunks:
        prompt = f"Summarize this in one sentence:\n{chunk}"
        summary = generate_chunk(prompt, max_new_tokens=None)  # No fixed limit
        chunk_summaries.append(summary)

    combined_summary = "\n".join(chunk_summaries)

    final_prompt = (
        f"Here is a summary of the large input text:\n{combined_summary}\n\n"
        f"Now, {second_text}"
    )

    output = generate_chunk(final_prompt, max_new_tokens=None)  # No fixed limit
    return output

# === Example usage ===

if __name__ == "__main__":
    large_text = """Paste your large input text here."""
    second_text = "Please summarize and answer the question: Is the product safe to use?"

    result = llm_process(large_text, second_text)
    print("=== Generated Output ===")
    print(result)
