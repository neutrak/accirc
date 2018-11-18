//printf
#include <stdio.h>

//rand()
#include <time.h>
#include <stdlib.h>

//dlist functions
#include "dlist.h"

#define TRUE 1
#define FALSE 0

#define TEST_LIST_SIZE 256
#define TEST_DELETE_COUNT 128

int dlist_test(char be_verbose){
	int failure_count=0;
	
	//create a list placeholder
	dlist_entry *test_list=NULL;
	
	//create a list
	int n;
	for(n=0;n<TEST_LIST_SIZE;n++){
		int *data=malloc(sizeof(int));
		(*data)=n;
		test_list=dlist_append(test_list,data);
	}
	
	//output list contents
	for(n=0;n<TEST_LIST_SIZE;n++){
		dlist_entry *item=dlist_get_entry(test_list,n);
		if(n!=(*((int*)item->data))){
			printf("item number %i is %i\n",n,*((int*)item->data));
			failure_count++;
		}
	}
	
	//delete a few items at random
	for(n=0;n<TEST_DELETE_COUNT;n++){
		int rand_idx=(rand()%(TEST_LIST_SIZE-n));
		if(be_verbose){
			printf("deleting item number %i\n",rand_idx);
		}
		test_list=dlist_delete_entry(test_list,rand_idx,TRUE);
	}
	
	//output list contents again
	for(n=0;n<(TEST_LIST_SIZE-TEST_DELETE_COUNT);n++){
		dlist_entry *item=dlist_get_entry(test_list,n);
		if(be_verbose){
			printf("item number %i is %i\n",n,*((int*)item->data));
		}
	}
	
	//ensure that the size of the list is what it should be, accounting for deleted items
	int length=dlist_length(test_list);
	if(length!=(TEST_LIST_SIZE-TEST_DELETE_COUNT)){
		printf("dlist_length is %i; should be %i\n",length,TEST_LIST_SIZE-TEST_DELETE_COUNT);
		failure_count++;
	}

	//deepcopy
	dlist_entry *test_list_copy=dlist_deep_copy(test_list,sizeof(int));
	
 	//compare the original list with the data-wise copy; they should be exactly the same!
	dlist_entry *cmp=test_list;
	dlist_entry *copy_cmp=test_list_copy;
	while(cmp!=NULL){
		int test_item=*((int*)(cmp->data));
		int test_item_copy=*((int*)(copy_cmp->data));
		if(test_item!=test_item_copy){
			printf("deep copy error, %i != %i\n",test_item,test_item_copy);
			failure_count++;
		}
		
		cmp=cmp->next;
		copy_cmp=copy_cmp->next;
	}
	
	length=dlist_length(test_list_copy);
	if(length!=(TEST_LIST_SIZE-TEST_DELETE_COUNT)){
		printf("copy length is %i; should be %i\n",length,TEST_LIST_SIZE-TEST_DELETE_COUNT);
		failure_count++;
	}
	
	//perform a bunch of moves on the test list
	for(n=0;(n<(TEST_DELETE_COUNT))&&(n<length);n++){
		int old_idx=(rand()%length);
		int ins_idx=(rand()%length);
		int new_idx=dlist_move_node(&test_list,old_idx,ins_idx);
		
		//if the new index is higher than the old index then subtract one
		//because it means the old index included a pass over the node which has now moved
		if(ins_idx>old_idx){
			ins_idx--;
		}
		
		if(new_idx!=ins_idx){
			printf("move node error; new_idx should be %i but is %i instead\n",ins_idx,new_idx);
			failure_count++;
		}
	}

	//peform a bunch of swaps on the test list
	for(n=0;n<(length*4);n++){
		int a_idx=(rand()%length);
		int b_idx=(rand()%length);
		
		int a_data=(*(int *)(dlist_get_entry(test_list,a_idx)->data));
		int b_data=(*(int *)(dlist_get_entry(test_list,b_idx)->data));
		
		test_list=dlist_swap(test_list,a_idx,b_idx);
		
		if((*(int *)(dlist_get_entry(test_list,a_idx)->data))!=b_data){
			printf("swap failed; item at index %i was not %i\n",a_idx,b_data);
			failure_count++;
		}
		if((*(int *)(dlist_get_entry(test_list,b_idx)->data))!=a_data){
			printf("swap failed; item at index %i was not %i\n",b_idx,a_data);
			failure_count++;
		}
	}
	
	//free the dlists
	dlist_free(test_list,TRUE);
	dlist_free(test_list_copy,TRUE);

	return failure_count;
}

int main(int argc, char *argv[]){
	srand(time(NULL));
	
	printf("running dlist (doubly-linked list) test... (for best results run in valgrind!)\n");
	int failure_count=dlist_test(FALSE);
	printf("dlist (doubly-linked list) test complete; %i failures\n",failure_count);
	
	//NOTE: 0 failures means success :)
	return failure_count;
}

