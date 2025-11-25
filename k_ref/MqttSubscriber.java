```java
package org.example;

import org.eclipse.paho.client.mqttv3.*;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;

public class MqttSubscriber {

    // Topic must match the publisher
//    private static final String TOPIC = "sensor/data";
    private static final String TOPIC = "moj_uzytkownik_123/#";
    // The broker URL, matching Docker open ports
//    private static final String BROKER = "tcp://localhost:1883";
    private static final String BROKER = "tcp://192.168.137.1:1883";
    // A unique client ID for this subscriber
    private static final String CLIENT_ID = "my_subscriber_id_123";

    public static void main(String[] argv) {
        MemoryPersistence persistence = new MemoryPersistence();

        try {
            MqttClient client = new MqttClient(BROKER, CLIENT_ID, persistence);
            MqttConnectOptions connOpts = new MqttConnectOptions();
            connOpts.setCleanSession(true);

            // Set username and password
            connOpts.setUserName("guest");
            connOpts.setPassword("guest".toCharArray());

            System.out.println("Connecting to broker: " + BROKER);
            client.connect(connOpts);
            System.out.println("Connected.");

            // Set the callback for message arrival
            client.setCallback(new MqttCallback() {
                @Override
                public void connectionLost(Throwable cause) {
                    System.out.println("Connection lost! " + cause.getMessage());
                    cause.printStackTrace();
                }

                @Override
                public void messageArrived(String topic, MqttMessage message) throws Exception {
                    String receivedMessage = new String(message.getPayload());
                    System.out.println(" [x] Received: '" + receivedMessage + "' on topic '" + topic + "'");
                }

                @Override
                public void deliveryComplete(IMqttDeliveryToken token) {
                    // Not used for subscriber
                }
            });

            // Subscribe to the topic
            client.subscribe(TOPIC);
            System.out.println(" [*] Subscribed to topic: " + TOPIC);
            System.out.println(" [*] Waiting for messages...");

            // Keep the application running to receive messages
            // In a real app, you'd have better logic here.
            // For this example, it just waits indefinitely.
            // We can't exit main, or the program will terminate.
            // new Object().wait(); // This is one way, but not ideal
            // A simple loop is fine for a demo
            // Or just let the main thread sleep
            // while(true) {
            //     Thread.sleep(1000);
            // }

        } catch (MqttException me) {
            System.out.println("MQTT Error:");
            System.out.println("reason " + me.getReasonCode());
            System.out.println("msg " + me.getMessage());
            System.out.println("loc " + me.getLocalizedMessage());
            System.out.println("cause " + me.getCause());
            System.out.println("excep " + me);
            me.printStackTrace();
        }
    }
}

//docker run -d --name rabbitmq-mqtt -p 1883:1883 -p 15672:15672 -p 5672:5672 rabbitmq:3-management
//
//docker exec rabbitmq-mqtt rabbitmq-plugins enable rabbitmq_mqtt
//
// mvn compile exec:java -Psubscriber
//docker exec rabbitmq-mqtt rabbitmq-plugins list
//
//
//docker run -d --name rabbitmq-mqtt -p 0.0.0.0:1883:1883 -p 0.0.0.0:15672:15672 -p 0.0.0.0:5672:5672 rabbitmq:3-management
```