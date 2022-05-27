import aiohttp
@event_trigger("esp32_ai_event")
async def handle_event(image=None):
    if(image is None):
        log.warning(f"no image")
        return
    url = "http://192.168.1.101:2137"
    try:
        async with aiohttp.ClientSession() as session:
            async with session.post(url, json={'image': image}) as resp:
                data = resp.json()
                event.fire("esp32_ai_response", data=data)
    except:
        event.fire("esp32_ai_response", data={"error": True})