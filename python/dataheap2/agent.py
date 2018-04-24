from abc import abstractmethod
import asyncio
import json
import traceback
import uuid

import aio_pika

from .logging import logger
from .rpc import RPCBase


def panic(loop, context):
    logger.error("exception in event loop: {}".format(context['message']))
    if context['exception']:
        print(context['exception'])
        # TODO figure out how to logger
        traceback.print_tb(context['exception'].__traceback__)
    loop.stop()


class Agent(RPCBase):
    def __init__(self, token, management_url):
        self.token = token

        self._management_url = management_url
        self._management_broadcast_exchange_name = 'dh2.broadcast'
        self._management_exchange_name = 'dh2.management'

        self._management_connection = None
        self._management_channel = None

        self._management_agent_queue = None

        self._management_broadcast_exchange = None
        self._management_exchange = None

        self._rpc_response_handlers = dict()
        logger.debug('Agent initialized')

    @property
    @abstractmethod
    def name(self):
        pass

    def make_correlation_id(self):
        return 'dh2-rpc-py-{}-{}'.format(self.name, uuid.uuid4().hex)

    @property
    def event_loop(self):
        return asyncio.get_event_loop()

    def run(self, die_on_exception=True):
        loop = asyncio.get_event_loop()
        if die_on_exception:
            loop.set_exception_handler(panic)
        loop.create_task(self.connect())
        loop.run_forever()

    async def connect(self):
        logger.info("establishing management connection to {}", self._management_url)

        self._management_connection = await aio_pika.connect_robust(self._management_url)
        self._management_channel = await self._management_connection.channel()
        self._management_agent_queue = await self._management_channel.declare_queue(
            '{}-rpc'.format(self.name), exclusive=True)

    async def _management_consume(self):
        logger.info('starting RPC consume')
        await self._management_agent_queue.consume(self.handle_management_message)

    async def _rpc(self, function, response_callback,
                   exchange: aio_pika.Exchange, routing_key: str,
                   arguments=None, timeout=10, cleanup_on_response=True):
        logger.info('sending RPC {}, exchange: {}, rk: {}, arguments: {}',
                    function, exchange.name, routing_key, arguments)

        if arguments is None:
            arguments = dict()
        arguments['function'] = function
        body = json.dumps(arguments).encode()
        correlation_id = self.make_correlation_id()
        msg = aio_pika.Message(body=body, correlation_id=correlation_id,
                               app_id=self.token,
                               reply_to=self._management_agent_queue.name,
                               content_type='application/json')
        self._rpc_response_handlers[correlation_id] = (response_callback, cleanup_on_response)
        await exchange.publish(msg, routing_key=routing_key)

        if timeout:
            def cleanup():
                try:
                    del self._rpc_response_handlers[correlation_id]
                except KeyError:
                    pass
            self.event_loop.call_later(timeout, cleanup)

    async def handle_management_message(self, message: aio_pika.Message):
        body = message.body.decode()
        from_token = message.app_id
        correlation_id = message.correlation_id.decode()

        logger.info('received message from {}, correlation id: {}, reply_to: {}\n{}',
                    from_token, correlation_id, message.reply_to, body)
        arguments = json.loads(body)
        arguments['from_token'] = from_token

        if 'function' in arguments:
            logger.debug('message is an RPC')
            response = await self.rpc_dispatch(**arguments)
            if response is None:
                response = dict()
            await self._management_channel.default_exchange.publish(
                aio_pika.Message(body=json.dumps(response).encode(),
                                 correlation_id=correlation_id,
                                 content_type='application/json',
                                 app_id=self.token),
                routing_key=message.reply_to)
        else:
            logger.debug('message is an RPC response')
            try:
                handler, cleanup = self._rpc_response_handlers[correlation_id]
            except KeyError:
                logger.error('received RPC response with unknown correlation id {} from {}',
                             correlation_id, from_token)
            if cleanup:
                del self._rpc_response_handlers[correlation_id]

            await handler(**arguments)
