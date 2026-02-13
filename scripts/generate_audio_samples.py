#!/usr/bin/env python3
"""
Generate audio samples for offline commands using Microsoft Edge TTS
Creates stories, jokes, and bedtime stories in OGG format for SD card playback
"""

import asyncio
import edge_tts
import subprocess
import os
from pathlib import Path

# Output directory structure
OUTPUT_DIR = Path("sdcard")
STORIES_DIR = OUTPUT_DIR / "stories"
JOKES_DIR = OUTPUT_DIR / "jokes"
GOODNIGHT_DIR = OUTPUT_DIR / "goodnight"

# Edge TTS voice (US English female voice - sounds friendly for kids)
VOICE = "en-US-AriaNeural"  # Options: en-US-AriaNeural, en-US-GuyNeural, en-US-JennyNeural

# Story samples (30-60 seconds each)
STORIES = [
    {
        "name": "the_brave_robot",
        "text": """Once upon a time, there was a brave little robot named Bolt.
        Bolt lived in a workshop with his friends, helping people build amazing things.
        One day, a small kitten got stuck on top of a very tall building.
        Everyone was worried, but Bolt said, 'Don't worry, I can help!'
        With his extending arms and rocket feet, Bolt flew up to save the kitten.
        The kitten was so happy! Everyone cheered for Bolt, the bravest robot in town.
        And from that day on, Bolt knew that being brave means helping others when they need you most."""
    },
    {
        "name": "magic_garden",
        "text": """In a magical garden, there lived a tiny seed named Sunny.
        Sunny dreamed of becoming a beautiful flower one day.
        Each morning, the gentle rain would water Sunny, and the warm sun would shine down.
        Sunny worked very hard, pushing through the dark soil bit by bit.
        After many days, Sunny finally popped out of the ground!
        With time and care, Sunny grew into the most beautiful sunflower the garden had ever seen.
        All the butterflies and bees came to visit, and Sunny smiled, knowing that patience and hard work make dreams come true."""
    },
    {
        "name": "friendly_dragon",
        "text": """High in the mountains lived a friendly dragon named Spark.
        Unlike other dragons who were scary, Spark loved to help people.
        One winter, the village below ran out of firewood and everyone was cold.
        Spark had an idea! He used his gentle fire breath to warm all the houses.
        The villagers were so grateful, they threw a big party for Spark.
        They learned that you shouldn't judge someone by how they look.
        Spark and the villagers became best friends, and the dragon visited them every winter to keep everyone warm and cozy."""
    }
]

# Joke samples (30-60 seconds each)
JOKES = [
    {
        "name": "joke_1",
        "text": """Here's a funny one for you!
        Why don't scientists trust atoms? Because they make up everything!
        And here's another: What do you call a bear with no teeth? A gummy bear!
        One more: Why did the scarecrow win an award? Because he was outstanding in his field!
        I hope those made you smile! Remember, laughter is the best medicine, unless you're actually sick, then you should probably see a doctor!"""
    },
    {
        "name": "joke_2",
        "text": """Let me tell you some silly jokes!
        What do you call cheese that isn't yours? Nacho cheese!
        Why can't a bicycle stand up by itself? Because it's two tired!
        What did one wall say to the other wall? I'll meet you at the corner!
        And finally, why did the cookie go to the doctor? Because it felt crumbly!
        Keep smiling and laughing every day!"""
    },
    {
        "name": "joke_3",
        "text": """Time for some giggles!
        What do you call a dinosaur that crashes his car? Tyrannosaurus wrecks!
        Why don't eggs tell jokes? They'd crack each other up!
        What do you call a sleeping bull? A bulldozer!
        And here's a good one: Why did the math book look so sad? Because it had too many problems!
        Remember, a day without laughter is a day wasted!"""
    }
]

# Bedtime stories (30-60 seconds each)
GOODNIGHT_STORIES = [
    {
        "name": "sleepy_moon",
        "text": """It's time to rest, little one.
        Look up at the sleepy moon in the sky. The moon is yawning, ready for bed.
        All the stars are twinkling softly, like little nightlights just for you.
        The gentle wind is singing a quiet lullaby, rustling through the trees.
        All the animals are curled up in their cozy homes, dreaming sweet dreams.
        Close your eyes and imagine floating on a soft, fluffy cloud.
        Tomorrow will bring new adventures, but now it's time to sleep. Good night, sleep tight, and have the sweetest dreams."""
    },
    {
        "name": "dream_boat",
        "text": """Close your eyes and let's sail away on the dream boat.
        The dream boat is soft and gentle, rocking slowly on calm waters.
        Above you, the sky is filled with twinkling stars that smile down at you.
        A friendly owl hoots softly from a nearby tree, saying good night.
        The water makes gentle splashing sounds, soothing and peaceful.
        As you drift along, everything becomes quiet and calm.
        Your breathing slows down, and your body feels relaxed and comfortable.
        Sweet dreams are waiting for you just ahead. Good night, and sleep peacefully."""
    },
    {
        "name": "forest_lullaby",
        "text": """In the quiet forest, all the creatures are going to sleep.
        The baby rabbits are snuggled in their burrow, warm and safe.
        The little birds have tucked their heads under their wings.
        Even the busy squirrels have stopped to rest in their tree homes.
        The forest is peaceful and still, with only the sound of gentle breathing.
        The trees stand tall like guardians, watching over everyone through the night.
        You are safe and loved, just like all the animals in the forest.
        Rest now, and let sleep carry you to wonderful dreams. Good night, little one."""
    }
]

async def generate_audio(text: str, output_mp3: Path):
    """Generate MP3 audio from text using Edge TTS"""
    print(f"  Generating speech for: {output_mp3.name}")
    communicate = edge_tts.Communicate(text, VOICE)
    await communicate.save(str(output_mp3))

def convert_mp3_to_ogg(input_mp3: Path, output_ogg: Path):
    """Convert MP3 to OGG using ffmpeg with Xiaozhi-compatible parameters"""
    print(f"  Converting to OGG: {output_ogg.name}")
    cmd = [
        "ffmpeg", "-i", str(input_mp3),
        "-c:a", "libopus",      # Opus codec
        "-b:a", "16k",          # 16kbps bitrate
        "-ac", "1",             # Mono
        "-ar", "16000",         # 16kHz sample rate
        "-frame_duration", "60", # 60ms frame duration
        "-y",                   # Overwrite output
        str(output_ogg)
    ]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

async def generate_samples(samples: list, output_dir: Path, category: str):
    """Generate audio samples for a category"""
    print(f"\n{'='*60}")
    print(f"Generating {category}...")
    print(f"{'='*60}")

    output_dir.mkdir(parents=True, exist_ok=True)

    for idx, sample in enumerate(samples, 1):
        print(f"\n[{idx}/{len(samples)}] Processing: {sample['name']}")

        # Temporary MP3 file
        temp_mp3 = output_dir / f"{sample['name']}.mp3"
        final_ogg = output_dir / f"{sample['name']}.ogg"

        try:
            # Generate MP3 using Edge TTS
            await generate_audio(sample['text'], temp_mp3)

            # Convert MP3 to OGG
            convert_mp3_to_ogg(temp_mp3, final_ogg)

            # Remove temporary MP3
            temp_mp3.unlink()

            print(f"  ✓ Success: {final_ogg}")

        except Exception as e:
            print(f"  ✗ Error: {e}")
            if temp_mp3.exists():
                temp_mp3.unlink()

async def main():
    print("""
╔═══════════════════════════════════════════════════════════╗
║  Xiaozhi Audio Sample Generator                          ║
║  Generates stories, jokes, and bedtime audio for SD card ║
╚═══════════════════════════════════════════════════════════╝
""")

    print(f"Voice: {VOICE}")
    print(f"Output directory: {OUTPUT_DIR.absolute()}")

    # Generate all categories
    await generate_samples(STORIES, STORIES_DIR, "Stories")
    await generate_samples(JOKES, JOKES_DIR, "Jokes")
    await generate_samples(GOODNIGHT_STORIES, GOODNIGHT_DIR, "Bedtime Stories")

    print(f"\n{'='*60}")
    print("✓ All samples generated successfully!")
    print(f"{'='*60}")
    print(f"\nOutput structure:")
    print(f"  {OUTPUT_DIR}/")
    print(f"    ├── stories/    ({len(STORIES)} files)")
    print(f"    ├── jokes/      ({len(JOKES)} files)")
    print(f"    └── goodnight/  ({len(GOODNIGHT_STORIES)} files)")
    print(f"\nCopy the '{OUTPUT_DIR}' folder to your SD card root directory.")
    print()

if __name__ == "__main__":
    # Check if ffmpeg is available
    try:
        subprocess.run(["ffmpeg", "-version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("ERROR: ffmpeg not found!")
        print("Please install ffmpeg:")
        print("  macOS:   brew install ffmpeg")
        print("  Ubuntu:  sudo apt install ffmpeg")
        print("  Windows: Download from https://ffmpeg.org/download.html")
        exit(1)

    # Run async main
    asyncio.run(main())
