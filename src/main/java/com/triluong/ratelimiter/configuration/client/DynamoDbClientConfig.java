package com.triluong.ratelimiter.configuration.client;


import com.amazonaws.auth.AWSCredentials;
import com.amazonaws.auth.BasicAWSCredentials;
import com.amazonaws.services.dynamodbv2.AmazonDynamoDB;
import com.amazonaws.services.dynamodbv2.AmazonDynamoDBClient;
import com.amazonaws.services.dynamodbv2.document.DynamoDB;
import io.micrometer.common.util.StringUtils;
import org.socialsignin.spring.data.dynamodb.repository.config.EnableDynamoDBRepositories;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;
import org.springframework.beans.factory.annotation.Value;

/**
 *
 * Contains the connection and initialization of the dynamoDb client
 * for the service to use
 *
 * Annotate with the Configuration to let Spring know that this is
 * a config class
 *
 * **/
@Configuration
@EnableDynamoDBRepositories(basePackages = "com.triluong.ratelimiter.repository")
public class DynamoDbClientConfig {

    @Value("${amazon.dynamodb.endpoint}")
    private String amazonDynamoDbEndpoint;

    @Value("${amazon.aws.accesskey}")
   private String amazonAwsAccessKey;

    @Value("${amazon.aws.secretkey}")
   private String amazonAwsSecretKey;

   @Bean
   public AmazonDynamoDB getAmazonDbClient() {
       AmazonDynamoDB client = new AmazonDynamoDBClient(getAwsCredential());
       //setting endpoint to client
       if (!StringUtils.isEmpty(amazonDynamoDbEndpoint)) {
           client.setEndpoint(amazonDynamoDbEndpoint);
       }

       return client;

   }

   @Bean
   public DynamoDB getDynamoDBClient() {
        return new DynamoDB(getAmazonDbClient());
   }


   //Function to get aws credential to authenticate with the AmazonDynamoDB service
   @Bean
   public AWSCredentials getAwsCredential() {
       return new BasicAWSCredentials(
               amazonAwsAccessKey, amazonAwsSecretKey);
   }
}
