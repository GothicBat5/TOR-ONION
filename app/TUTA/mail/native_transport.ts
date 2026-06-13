import { defer, DeferredObject } from "../../../platform-kit/utils"
import { assertMainOrNode } from "../../../platform-kit/app-env"
import { Transport } from "../shared/MessageTypes"
import { decodeNativeMessage, encodeNativeMessage, JsMessageHandler, NativeMessage } from "../common/NativeLineProtocol.js"

assertMainOrNode()

export class AndroidNativeTransport implements Transport<NativeRequestType, JsRequestType> {
	private messageHandler: JsMessageHandler | null = null
	private deferredPort: DeferredObject<MessagePort> = defer()

	constructor(private readonly window: Window) {}

	start() {

		this.window.onmessage = (message: MessageEvent) => {

			const port = message.ports[0]
			port.onmessage = (messageEvent: MessageEvent) => {
				const handler = this.messageHandler

				if (handler) 
        {
					const response = decodeNativeMessage(messageEvent.data)
					handler(response)
				}
			}

			this.deferredPort.resolve(port)
		}

		const nativeApp = this.window.nativeApp as NativeApp
		nativeApp.startWebMessageChannel()
	}

	postMessage(message: NativeMessage): void {
		const encoded = encodeNativeMessage(message)
		this.deferredPort.promise.then((port) => port.postMessage(encoded))
	}

	setMessageHandler(handler: JsMessageHandler): void {
		this.messageHandler = handler
	}
}
