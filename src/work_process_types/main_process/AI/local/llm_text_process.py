
from transformers import AutoTokenizer, AutoModelForCausalLM, BitsAndBytesConfig, StoppingCriteria, StoppingCriteriaList
import torch
from langchain.text_splitter import RecursiveCharacterTextSplitter
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import time

# === Default Configuration ===
buffer_tokens = 256
enable_streaming_default = False  # default off
model_id = "microsoft/Phi-4-mini-instruct"

# === Load tokenizer ===
tokenizer = AutoTokenizer.from_pretrained(model_id, trust_remote_code=True)
tokenizer.model_max_length = getattr(tokenizer, "model_max_length", 2048)

# === Load model with auto quantization / multi-GPU fallback ===
use_quant = True
try:
    bnb_config = BitsAndBytesConfig(
        load_in_4bit=True,
        bnb_4bit_use_double_quant=True,
        bnb_4bit_compute_dtype=torch.float16,
        bnb_4bit_quant_type="nf4"
    ) if use_quant else None

    device_map = "auto" if torch.cuda.is_available() else None
    model = AutoModelForCausalLM.from_pretrained(
        model_id,
        trust_remote_code=True,
        quantization_config=bnb_config,
        device_map=device_map,
        torch_dtype=torch.float16
    )
    print("Using 4-bit quantization")
except Exception:
    use_quant = False
    model = AutoModelForCausalLM.from_pretrained(
        model_id,
        trust_remote_code=True,
        device_map="auto" if torch.cuda.is_available() else None,
        torch_dtype=torch.float16
    )
    print("4-bit quantization not supported, using FP16")

# === Utility Functions ===

def get_safe_chunk_tokens(buffer_tokens=buffer_tokens, safety_factor=0.8):
    """Estimate safe max tokens per chunk based on GPU memory"""
    if torch.cuda.is_available():
        device = torch.device("cuda")
        prop = torch.cuda.get_device_properties(device)
        total_mem = prop.total_memory
        reserved = torch.cuda.memory_reserved(device)
        free_mem = total_mem - reserved
        bytes_per_token = 2 if use_quant else 4
        est_max_tokens = int((free_mem * safety_factor) / bytes_per_token)
        return min(est_max_tokens, tokenizer.model_max_length - buffer_tokens)
    else:
        return min(1024, tokenizer.model_max_length - buffer_tokens)

def estimate_parallel_chunks(max_tokens):
    """Estimate number of parallel chunks based on memory"""
    if torch.cuda.is_available():
        device = torch.device("cuda")
        prop = torch.cuda.get_device_properties(device)
        total_mem = prop.total_memory
        reserved = torch.cuda.memory_reserved(device)
        free_mem = total_mem - reserved
        bytes_per_token = 2 if use_quant else 4
        chunk_mem = max_tokens * bytes_per_token
        max_parallel = max(1, int(free_mem * 0.8 / chunk_mem))
        return min(max_parallel, torch.cuda.device_count())
    else:
        return 2

def split_text_into_chunks(text, safety_factor=0.8):
    max_tokens = get_safe_chunk_tokens(buffer_tokens=buffer_tokens, safety_factor=safety_factor)
    splitter = RecursiveCharacterTextSplitter(
        chunk_size=max_tokens,
        chunk_overlap=max_tokens // 8,
        length_function=lambda x: len(tokenizer.encode(x, add_special_tokens=False)),
    )
    return splitter.split_text(text), max_tokens

def generate_text(prompt, max_new_tokens=None, stream=False, file_handle=None, callback=None,
                  temperature=0.0, top_p=0.95, repetition_penalty=1.0, do_sample=False):
    inputs = tokenizer(prompt, return_tensors="pt", truncation=True).to(model.device)
    generate_kwargs = {
        "pad_token_id": tokenizer.eos_token_id,
        "eos_token_id": tokenizer.eos_token_id,
        "max_new_tokens": max_new_tokens or 512,
        "temperature": temperature,
        "top_p": top_p,
        "repetition_penalty": repetition_penalty,
        "do_sample": do_sample
    }
    generate_kwargs.update(inputs)

    if stream:
        class StreamTokens(StoppingCriteria):
            def __init__(self, callback=None, file_handle=None):
                self.callback = callback
                self.file_handle = file_handle
            def __call__(self, input_ids, scores, **kwargs):
                token_id = input_ids[0, -1].item()
                text = tokenizer.decode([token_id])
                if self.callback:
                    self.callback(text)
                if self.file_handle:
                    self.file_handle.write(text)
                    self.file_handle.flush()
                else:
                    print(text, end='', flush=True)
                return False

        stopping_criteria = StoppingCriteriaList([StreamTokens(callback, file_handle)])
        try:
            model.generate(**generate_kwargs, stopping_criteria=stopping_criteria)
        except RuntimeError as e:
            if "out of memory" in str(e):
                torch.cuda.empty_cache()
                time.sleep(1)
                model.generate(**generate_kwargs, stopping_criteria=stopping_criteria)
        return ""
    else:
        try:
            outputs = model.generate(**generate_kwargs)
        except RuntimeError as e:
            if "out of memory" in str(e):
                torch.cuda.empty_cache()
                time.sleep(1)
                outputs = model.generate(**generate_kwargs)
        response = tokenizer.decode(outputs[0], skip_special_tokens=True)
        return response[len(prompt):].strip()

def summarize_chunk(chunk, summary_max_tokens=128):
    prompt = f"Summarize this in one concise sentence:\n{chunk}"
    summary = generate_text(prompt, max_new_tokens=summary_max_tokens)
    del prompt
    torch.cuda.empty_cache()
    return summary

# === Hybrid Pipeline ===
def llm_process_hybrid(
    input_path=None,
    input_text=None,
    second_path=None,
    second_text=None,
    output_path=None,
    stream=enable_streaming_default,
    callback=None,
    temperature=0.0,
    top_p=0.95,
    repetition_penalty=1.0,
    do_sample=False,
    max_new_tokens=None,
    safety_factor=0.8,
    summary_max_tokens=128,
    max_tokens_per_iteration=512
):
    # Load main input
    if input_text is None:
        if input_path is None or not os.path.exists(input_path):
            raise FileNotFoundError("Input file not found and input_text not provided")
        with open(input_path, "r", encoding="utf-8") as f:
            input_text = f.read()

    # Load second input
    if second_text is None:
        if second_path is not None and os.path.exists(second_path):
            with open(second_path, "r", encoding="utf-8") as f:
                second_text = f.read()
        else:
            second_text = ""

    # Chunking and summarization
    chunks, max_tokens_per_chunk = split_text_into_chunks(input_text, safety_factor=safety_factor)
    max_parallel = min(estimate_parallel_chunks(max_tokens_per_chunk), len(chunks))

    chunk_summaries = [None] * len(chunks)
    with ThreadPoolExecutor(max_workers=max_parallel) as executor:
        futures = {executor.submit(summarize_chunk, chunk, summary_max_tokens): i for i, chunk in enumerate(chunks)}
        for future in as_completed(futures):
            idx = futures[future]
            try:
                chunk_summaries[idx] = future.result()
            except Exception as e:
                print(f"Error summarizing chunk {idx}: {e}")
                chunk_summaries[idx] = ""

    combined_summary = "\n".join(chunk_summaries)

    # Multi-step / chain-of-thought reasoning
    final_prompt = f"Step 1: Analyze summary:\n{combined_summary}\n\nStep 2: {second_text}\n"
    result = "" if not output_path else None
    context = final_prompt

    file_handle = open(output_path, "a", encoding="utf-8") if output_path else None

    while True:
        new_text = generate_text(
            context,
            max_new_tokens=max_new_tokens or max_tokens_per_iteration,
            stream=stream,
            file_handle=file_handle,
            callback=callback,
            temperature=temperature,
            top_p=top_p,
            repetition_penalty=repetition_penalty,
            do_sample=do_sample
        )
        if not new_text.strip() and not stream:
            break

        if stream or output_path:
            context = context[-(tokenizer.model_max_length - buffer_tokens):]
        else:
            result += new_text
            context = result[-(tokenizer.model_max_length - buffer_tokens):]

        del new_text
        torch.cuda.empty_cache()

    if file_handle:
        file_handle.close()

    return "" if (stream or output_path) else result

# === Example Usage ===
if __name__ == "__main__":
    input_file = "input.txt"
    second_file = "question.txt"
    output_file = None

    def live_callback(token):
        print(token, end='', flush=True)

    result = llm_process_hybrid(
        input_path=input_file,
        second_path=second_file,
        output_path=output_file,
        stream=False,
        callback=live_callback,
        temperature=0.7,
        top_p=0.9,
        repetition_penalty=1.1,
        do_sample=True,
        max_new_tokens=2048,
        safety_factor=0.85,
        summary_max_tokens=150,
        max_tokens_per_iteration=512
    )

    if result:
        print("\n=== Generated Output ===")
        print(result)
