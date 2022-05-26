import tensorflow as tf
import numpy as np
import base64
import io
import os
from tensorflow.keras.preprocessing import image
from tensorflow.keras.applications import imagenet_utils
from PIL import Image
from fastapi import Request, FastAPI
from fastapi.middleware.cors import CORSMiddleware
from uvicorn import run

filename = 'tempimage.jpg'
model = tf.keras.applications.mobilenet_v2.MobileNetV2()

app = FastAPI()

origins = ["*"]
methods = ["*"]
headers = ["*"]

app.add_middleware(
    CORSMiddleware, 
    allow_origins = origins,
    allow_credentials = True,
    allow_methods = methods,
    allow_headers = headers    
)

def b64toimg(s):
    try:
        img = Image.open(io.BytesIO(base64.b64decode(s)))
        img.save(filename, 'jpeg')
        return True
    except:
        return False

def processImage(imagestr):
    if(b64toimg(imagestr) == False):
        return {"error": True}
    img = image.load_img(filename,target_size=(224,224))
    resizedimg = image.img_to_array(img)
    finalimg = np.expand_dims(resizedimg,axis=0)
    finalimg = tf.keras.applications.mobilenet_v2.preprocess_input(finalimg)
    finalimg.shape
    predictions = model.predict(finalimg)
    return {"predictions": list(map(lambda x: {"label":x[1], "prediction": float(x[2])}, imagenet_utils.decode_predictions(predictions)[0])) }

@app.post("/")
async def get_net_image_prediction(request: Request):
    return processImage((await request.json())["image"])

if __name__ == "__main__":
    port = int(os.environ.get('PORT', 2137))
    run(app, host="0.0.0.0", port=port)